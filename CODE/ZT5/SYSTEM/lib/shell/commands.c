/*
 * commands.c - 内置命令实现
 *
 * 包含所有内置 shell 命令和外部程序执行
 */

#include "zsh.h"

/* ============================================
 * 辅助函数
 * ============================================ */

/* 简单的八进制字符串转整数 */
static int octal_to_int(const char *s)
{
    int result = 0;
    while (*s >= '0' && *s <= '7') {
        result = result * 8 + (*s - '0');
        s++;
    }
    return result;
}

/* 格式化文件大小 */
static void format_size(uint64_t size, char *buf)
{
    if (size < 1024) {
        int_to_str((int)size, buf, 10);
        str_cat(buf, " B");
    } else if (size < 1024 * 1024) {
        int_to_str((int)(size / 1024), buf, 10);
        str_cat(buf, " K");
    } else if (size < 1024 * 1024 * 1024) {
        int_to_str((int)(size / (1024 * 1024)), buf, 10);
        str_cat(buf, " M");
    } else {
        int_to_str((int)(size / (1024 * 1024 * 1024)), buf, 10);
        str_cat(buf, " G");
    }
}

/* ============================================
 * cd - 改变目录
 * ============================================ */

int cmd_cd(char **argv, int argc)
{
    const char *path;

    if (argc < 2) {
        /* 无参数，回到家目录 */
        path = "/root";
    } else {
        path = argv[1];
    }

    /* 处理 ~ 路径 */
    char resolved[MAX_PATH];
    if (path[0] == '~') {
        str_cpy(resolved, "/root");
        if (path[1] == '/' || path[1] != '\0') {
            str_cat(resolved, path + 1);
        }
        path = resolved;
    }

    long ret = sys_chdir(path);
    if (ret < 0) {
        print("cd: ");
        print(path);
        println(": No such file or directory");
        return 1;
    }

    /* 更新当前目录 */
    sys_getcwd(g_shell.cwd, MAX_PATH);
    return 0;
}

/* ============================================
 * echo - 输出文本
 * ============================================ */

int cmd_echo(char **argv, int argc)
{
    for (int i = 1; i < argc; i++) {
        print(argv[i]);
        if (i < argc - 1) {
            print(" ");
        }
    }
    print("\n\r");
    return 0;
}

/* ============================================
 * pwd - 显示当前目录
 * ============================================ */

int cmd_pwd(char **argv, int argc)
{
    (void)argv;
    (void)argc;
    println(g_shell.cwd);
    return 0;
}

/* ============================================
 * ls - 列出目录内容 (带颜色区分文件/目录)
 * ============================================ */

int cmd_ls(char **argv, int argc)
{
    const char *path = ".";
    int show_long = 0;

    /* 解析参数 */
    for (int i = 1; i < argc; i++) {
        if (str_cmp(argv[i], "-l") == 0 || str_cmp(argv[i], "-la") == 0 || str_cmp(argv[i], "-al") == 0) {
            show_long = 1;
        } else if (argv[i][0] != '-') {
            path = argv[i];
        }
    }

    /* 打开目录 */
    long fd = sys_open(path, O_RDONLY, 0);
    if (fd < 0) {
        print("ls: ");
        print(path);
        println(": Cannot open directory");
        return 1;
    }

    /* 读取目录条目 */
    char buf[4096];
    int first = 1;

    while (1) {
        long nread = sys_readdir(fd, buf, 4096);
        if (nread <= 0) {
            break;
        }

        long offset = 0;
        while (offset < nread) {
            struct linux_dirent64 *dirent = (struct linux_dirent64*)(buf + offset);

            /* 跳过 . 和 .. */
            if (dirent->d_name[0] == '.') {
                if (dirent->d_name[1] == '\0' ||
                    (dirent->d_name[1] == '.' && dirent->d_name[2] == '\0')) {
                    offset += dirent->d_reclen;
                    continue;
                }
            }

            if (!first) {
                print("  ");
            }

            if (show_long) {
                /* 长格式: 类型 大小 名称 */
                char type_char = '-';
                if (dirent->d_type == DT_DIR) type_char = 'd';
                else if (dirent->d_type == DT_LNK) type_char = 'l';
                else if (dirent->d_type == DT_CHR) type_char = 'c';
                else if (dirent->d_type == DT_BLK) type_char = 'b';

                print_char(type_char);
                print(" ");

                /* 获取文件大小 */
                char full_path[MAX_PATH];
                path_join(full_path, path, dirent->d_name, MAX_PATH);
                struct zsh_stat st;
                if (sys_stat(full_path, &st) == 0) {
                    char size_buf[32];
                    format_size(st.st_size, size_buf);
                    /* 右对齐 */
                    int slen = str_len(size_buf);
                    for (int p = slen; p < 8; p++) print_char(' ');
                    print(size_buf);
                } else {
                    print("       ?");
                }
                print(" ");
            }

            /* 目录用蓝色，普通文件用默认色 */
            if (dirent->d_type == DT_DIR) {
                print("\033[1;34m");  /* 蓝色加粗 */
                print(dirent->d_name);
                print("/");
                print("\033[0m");     /* 恢复颜色 */
            } else if (dirent->d_type == DT_LNK) {
                print("\033[1;36m");  /* 青色加粗 */
                print(dirent->d_name);
                print("\033[0m");
            } else if (dirent->d_type == DT_CHR || dirent->d_type == DT_BLK) {
                print("\033[1;33m");  /* 黄色加粗 */
                print(dirent->d_name);
                print("\033[0m");
            } else {
                /* 检查是否可执行 (简化: .prd 后缀) */
                int name_len = 0;
                while (dirent->d_name[name_len]) name_len++;
                if (name_len > 4 &&
                    dirent->d_name[name_len-4] == '.' &&
                    (dirent->d_name[name_len-3] == 'p' || dirent->d_name[name_len-3] == 'P') &&
                    (dirent->d_name[name_len-2] == 'r' || dirent->d_name[name_len-2] == 'R') &&
                    (dirent->d_name[name_len-1] == 'd' || dirent->d_name[name_len-1] == 'D')) {
                    print("\033[1;32m");  /* 绿色加粗 */
                    print(dirent->d_name);
                    print("\033[0m");
                } else {
                    print(dirent->d_name);
                }
            }

            first = 0;
            offset += dirent->d_reclen;
        }
    }

    if (!first) {
        print("\n\r");
    }

    sys_close(fd);
    return 0;
}

/* ============================================
 * cat - 显示文件内容
 * ============================================ */

int cmd_cat(char **argv, int argc)
{
    if (argc < 2) {
        println("cat: missing file operand");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        long fd = sys_open(argv[i], O_RDONLY, 0);
        if (fd < 0) {
            print("cat: ");
            print(argv[i]);
            println(": No such file or directory");
            return 1;
        }

        char buf[512];
        while (1) {
            long n = sys_read(fd, buf, 512);
            if (n <= 0) break;
            sys_write(STDOUT_FD, buf, n);
        }

        sys_close(fd);
    }

    return 0;
}

/* ============================================
 * mkdir - 创建目录
 * ============================================ */

int cmd_mkdir(char **argv, int argc)
{
    if (argc < 2) {
        println("mkdir: missing operand");
        return 1;
    }

    long ret = sys_mkdir(argv[1], 0755);
    if (ret < 0) {
        print("mkdir: ");
        print(argv[1]);
        println(": Cannot create directory");
        return 1;
    }

    return 0;
}

/* ============================================
 * rm - 删除文件
 * ============================================ */

int cmd_rm(char **argv, int argc)
{
    if (argc < 2) {
        println("rm: missing operand");
        return 1;
    }

    int recursive = 0;
    const char *target = NULL;

    for (int i = 1; i < argc; i++) {
        if (str_cmp(argv[i], "-r") == 0 || str_cmp(argv[i], "-rf") == 0 || str_cmp(argv[i], "-fr") == 0) {
            recursive = 1;
        } else if (argv[i][0] != '-') {
            target = argv[i];
        }
    }

    if (!target) {
        println("rm: missing operand");
        return 1;
    }

    /* 检查是否是目录 */
    struct zsh_stat st;
    if (sys_stat(target, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR) {
        if (!recursive) {
            print("rm: ");
            print(target);
            println(": is a directory (use -r to remove)");
            return 1;
        }
        long ret = sys_rmdir(target);
        if (ret < 0) {
            print("rm: ");
            print(target);
            println(": Cannot remove directory");
            return 1;
        }
    } else {
        long ret = sys_unlink(target);
        if (ret < 0) {
            print("rm: ");
            print(target);
            println(": Cannot remove file");
            return 1;
        }
    }

    return 0;
}

/* ============================================
 * cp - 复制文件
 * ============================================ */

int cmd_cp(char **argv, int argc)
{
    if (argc < 3) {
        println("cp: missing file operand");
        println("Usage: cp <source> <destination>");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    long src_fd = sys_open(src, O_RDONLY, 0);
    if (src_fd < 0) {
        print("cp: ");
        print(src);
        println(": No such file or directory");
        return 1;
    }

    /* 检查目标是否是目录 */
    char dst_path[MAX_PATH];
    struct zsh_stat dst_st;
    if (sys_stat(dst, &dst_st) == 0 && (dst_st.st_mode & S_IFMT) == S_IFDIR) {
        /* 目标是目录，在目录下创建同名文件 */
        const char *basename = path_basename(src);
        path_join(dst_path, dst, basename, MAX_PATH);
    } else {
        str_ncpy(dst_path, dst, MAX_PATH - 1);
    }

    long dst_fd = sys_open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        print("cp: ");
        print(dst_path);
        println(": Cannot create file");
        sys_close(src_fd);
        return 1;
    }

    char buf[4096];
    while (1) {
        long n = sys_read(src_fd, buf, 4096);
        if (n <= 0) break;
        sys_write(dst_fd, buf, n);
    }

    sys_close(src_fd);
    sys_close(dst_fd);
    return 0;
}

/* ============================================
 * mv - 移动/重命名文件
 * ============================================ */

int cmd_mv(char **argv, int argc)
{
    if (argc < 3) {
        println("mv: missing file operand");
        println("Usage: mv <source> <destination>");
        return 1;
    }

    const char *src = argv[1];
    const char *dst = argv[2];

    /* 检查目标是否是目录 */
    char dst_path[MAX_PATH];
    struct zsh_stat dst_st;
    if (sys_stat(dst, &dst_st) == 0 && (dst_st.st_mode & S_IFMT) == S_IFDIR) {
        const char *basename = path_basename(src);
        path_join(dst_path, dst, basename, MAX_PATH);
    } else {
        str_ncpy(dst_path, dst, MAX_PATH - 1);
    }

    /* 先复制 */
    long src_fd = sys_open(src, O_RDONLY, 0);
    if (src_fd < 0) {
        print("mv: ");
        print(src);
        println(": No such file or directory");
        return 1;
    }

    long dst_fd = sys_open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        print("mv: ");
        print(dst_path);
        println(": Cannot create file");
        sys_close(src_fd);
        return 1;
    }

    char buf[4096];
    while (1) {
        long n = sys_read(src_fd, buf, 4096);
        if (n <= 0) break;
        sys_write(dst_fd, buf, n);
    }

    sys_close(src_fd);
    sys_close(dst_fd);

    /* 删除源文件 */
    sys_unlink(src);
    return 0;
}

/* ============================================
 * clear - 清屏
 * ============================================ */

int cmd_clear(char **argv, int argc)
{
    (void)argv;
    (void)argc;
    print("\033[2J\033[H");
    return 0;
}

/* ============================================
 * help - 显示帮助
 * ============================================ */

int cmd_help(char **argv, int argc)
{
    (void)argv;
    (void)argc;
    println("Zratro Shell (zsh) - Available commands:");
    println("");
    println("  cd [dir]        - Change directory");
    println("  pwd             - Print working directory");
    println("  ls [dir]        - List directory contents");
    println("  cat <file>      - Display file contents");
    println("  echo <text>     - Print text");
    println("  mkdir <dir>     - Create directory");
    println("  rm [-r] <file>  - Remove file/directory");
    println("  cp <src> <dst>  - Copy file");
    println("  mv <src> <dst>  - Move/rename file");
    println("  touch <file>    - Create empty file");
    println("  chmod <mode> <file> - Change file permissions");
    println("  whoami          - Display current user");
    println("  uname           - Display system information");
    println("  clear           - Clear screen");
    println("  help            - Show this help");
    println("  exit            - Exit shell");
    println("  sudo <command>  - Execute with elevated privileges");
    println("");
    println("Pipe and Redirection:");
    println("  cmd1 | cmd2     - Pipe output of cmd1 to cmd2");
    println("  cmd > file      - Redirect stdout to file");
    println("  cmd >> file     - Append stdout to file");
    println("  cmd < file      - Redirect stdin from file");
    println("  cmd 2> file     - Redirect stderr to file");
    println("  cmd &> file     - Redirect stdout+stderr to file");
    println("");
    println("Keyboard shortcuts:");
    println("  Up/Down         - Command history");
    println("  Left/Right      - Move cursor");
    println("  Home/End        - Jump to start/end of line");
    println("  Insert          - Toggle insert/overwrite mode");
    println("  Tab             - Auto-complete");
    println("  Ctrl+R          - Incremental history search");
    println("  Ctrl+C          - Cancel current line");
    println("  Ctrl+L          - Clear screen");
    println("  Ctrl+U          - Kill line to start");
    println("  Ctrl+K          - Kill line to end");
    println("  Ctrl+W          - Delete word backward");
    println("  Ctrl+A          - Move to start of line");
    println("  Ctrl+E          - Move to end of line");
    println("");
    println("Use external programs by typing their name.");
    return 0;
}

/* ============================================
 * exit - 退出 shell
 * ============================================ */

int cmd_exit(char **argv, int argc)
{
    (void)argv;
    (void)argc;
    g_shell.running = 0;
    return 0;
}

/* ============================================
 * whoami - 显示当前用户
 * ============================================ */

int cmd_whoami(char **argv, int argc)
{
    (void)argv;
    (void)argc;
    println(g_shell.username);
    return 0;
}

/* ============================================
 * uname - 显示系统信息
 * ============================================ */

int cmd_uname(char **argv, int argc)
{
    (void)argc;
    struct utsname buf;
    long ret = sys_uname(&buf);
    if (ret < 0) {
        println("uname: failed to get system information");
        return 1;
    }

    int show_all = 0;
    for (int i = 1; i < argc; i++) {
        if (str_cmp(argv[i], "-a") == 0) {
            show_all = 1;
            break;
        }
    }

    if (show_all) {
        print(buf.sysname);
        print(" ");
        print(buf.nodename);
        print(" ");
        print(buf.release);
        print(" ");
        print(buf.version);
        print(" ");
        println(buf.machine);
    } else {
        println(buf.sysname);
    }

    return 0;
}

/* ============================================
 * touch - 创建空文件
 * ============================================ */

int cmd_touch(char **argv, int argc)
{
    if (argc < 2) {
        println("touch: missing file operand");
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        /* 检查文件是否已存在 */
        struct zsh_stat st;
        if (sys_stat(argv[i], &st) == 0) {
            /* 文件已存在，不做修改 (简化版不更新时间戳) */
            continue;
        }

        /* 创建空文件 */
        long fd = sys_open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) {
            print("touch: ");
            print(argv[i]);
            println(": Cannot create file");
            return 1;
        }
        sys_close(fd);
    }

    return 0;
}

/* ============================================
 * chmod - 修改权限
 * ============================================ */

int cmd_chmod(char **argv, int argc)
{
    if (argc < 3) {
        println("chmod: missing operand");
        println("Usage: chmod <mode> <file>");
        return 1;
    }

    int mode;
    /* 支持八进制模式 (如 755) */
    if (argv[1][0] >= '0' && argv[1][0] <= '7') {
        mode = octal_to_int(argv[1]);
    } else {
        /* 简化: 只支持八进制 */
        println("chmod: only octal mode supported (e.g., 755)");
        return 1;
    }

    long ret = sys_chmod(argv[2], mode);
    if (ret < 0) {
        print("chmod: ");
        print(argv[2]);
        println(": Cannot change permissions");
        return 1;
    }

    return 0;
}

/* ============================================
 * sudo - 提权执行命令
 * ============================================ */

/* MD5 常量表 */
static const uint32_t md5_k[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};
static const uint32_t md5_s[64] = {
    7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
    5,9,14,20,5,9,14,20,5,9,14,20,5,9,14,20,
    4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
    6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21
};

static void compute_md5(const char *input, int plen, char hash[33])
{
    uint32_t h0=0x67452301, h1=0xefcdab89, h2=0x98badcfe, h3=0x10325476;
    int bit_len = plen * 8;
    int new_len = ((plen + 8) / 64 + 1) * 64;

    /* 使用栈上的缓冲区，限制密码长度 */
    unsigned char msg[256];
    for (int i = 0; i < 256; i++) msg[i] = 0;
    for (int i = 0; i < plen && i < 250; i++) msg[i] = (unsigned char)input[i];
    msg[plen] = 0x80;
    msg[new_len - 8] = (unsigned char)(bit_len & 0xff);
    msg[new_len - 7] = (unsigned char)((bit_len >> 8) & 0xff);
    msg[new_len - 6] = (unsigned char)((bit_len >> 16) & 0xff);
    msg[new_len - 5] = (unsigned char)((bit_len >> 24) & 0xff);

    for (int offset = 0; offset < new_len; offset += 64) {
        uint32_t M[16];
        for (int i = 0; i < 16; i++) {
            M[i] = (uint32_t)msg[offset+i*4] |
                   ((uint32_t)msg[offset+i*4+1]<<8) |
                   ((uint32_t)msg[offset+i*4+2]<<16) |
                   ((uint32_t)msg[offset+i*4+3]<<24);
        }
        uint32_t a=h0, b=h1, c=h2, d=h3;
        for (int i = 0; i < 64; i++) {
            uint32_t f, g;
            if (i < 16) { f = (b&c)|((~b)&d); g = i; }
            else if (i < 32) { f = (d&b)|((~d)&c); g = (5*i+1)%16; }
            else if (i < 48) { f = b^c^d; g = (3*i+5)%16; }
            else { f = c^(b|(~d)); g = (7*i)%16; }
            f = f + a + md5_k[i] + M[g];
            a = d; d = c; c = b;
            b = b + ((f << md5_s[i]) | (f >> (32 - md5_s[i])));
        }
        h0 += a; h1 += b; h2 += c; h3 += d;
    }

    const char hex[] = "0123456789abcdef";
    hash[0]  = hex[(h0>>4)&0xf];  hash[1]  = hex[h0&0xf];
    hash[2]  = hex[(h0>>12)&0xf]; hash[3]  = hex[(h0>>8)&0xf];
    hash[4]  = hex[(h0>>20)&0xf]; hash[5]  = hex[(h0>>16)&0xf];
    hash[6]  = hex[(h0>>28)&0xf]; hash[7]  = hex[(h0>>24)&0xf];
    hash[8]  = hex[(h1>>4)&0xf];  hash[9]  = hex[h1&0xf];
    hash[10] = hex[(h1>>12)&0xf]; hash[11] = hex[(h1>>8)&0xf];
    hash[12] = hex[(h1>>20)&0xf]; hash[13] = hex[(h1>>16)&0xf];
    hash[14] = hex[(h1>>28)&0xf]; hash[15] = hex[(h1>>24)&0xf];
    hash[16] = hex[(h2>>4)&0xf];  hash[17] = hex[h2&0xf];
    hash[18] = hex[(h2>>12)&0xf]; hash[19] = hex[(h2>>8)&0xf];
    hash[20] = hex[(h2>>20)&0xf]; hash[21] = hex[(h2>>16)&0xf];
    hash[22] = hex[(h2>>28)&0xf]; hash[23] = hex[(h2>>24)&0xf];
    hash[24] = hex[(h3>>4)&0xf];  hash[25] = hex[h3&0xf];
    hash[26] = hex[(h3>>12)&0xf]; hash[27] = hex[(h3>>8)&0xf];
    hash[28] = hex[(h3>>20)&0xf]; hash[29] = hex[(h3>>16)&0xf];
    hash[30] = hex[(h3>>28)&0xf]; hash[31] = hex[(h3>>24)&0xf];
    hash[32] = 0;
}

int cmd_sudo(char **argv, int argc)
{
    if (argc < 2) {
        println("Usage: sudo <command>");
        return 1;
    }

    /* 询问密码 */
    print("[sudo] Password for root: ");

    /* 读取密码 (无回显) */
    char password[64] = {0};
    int plen = 0;
    while (plen < 63) {
        char c;
        long n = sys_read(STDIN_FD, &c, 1);
        if (n <= 0) break;
        if (c == '\n' || c == '\r') {
            password[plen] = 0;
            break;
        }
        if (c == KEY_BACKSPACE || c == 127) {
            if (plen > 0) {
                plen--;
                password[plen] = 0;
            }
            continue;
        }
        if (c == KEY_CTRL_C) {
            print("\n\r");
            return 1;
        }
        password[plen++] = c;
    }
    print("\n\r");

    if (plen == 0) {
        println("sudo: No password provided");
        return 1;
    }

    /* 计算 MD5 哈希 */
    char hash[33];
    compute_md5(password, plen, hash);

    /* 调用 SYS_SUDO (196) */
    long ret = sys_sudo(hash);

    if (ret != 0) {
        println("sudo: Authentication failure");
        return 1;
    }

    println("sudo: Privileges elevated to root");

    /* 执行命令 */
    char cmd[MAX_CMD_LEN] = {0};
    int pos = 0;
    for (int i = 1; i < argc && pos < MAX_CMD_LEN - 2; i++) {
        if (pos > 0) cmd[pos++] = ' ';
        const char *s = argv[i];
        while (*s && pos < MAX_CMD_LEN - 1) cmd[pos++] = *s++;
    }
    cmd[pos] = 0;

    if (pos > 0) {
        return execute_command(cmd);
    }

    return 0;
}

/* ============================================
 * 内置命令表
 * ============================================ */

builtin_cmd_t builtins[] = {
    {"cd",    cmd_cd},
    {"echo",  cmd_echo},
    {"pwd",   cmd_pwd},
    {"ls",    cmd_ls},
    {"cat",   cmd_cat},
    {"mkdir", cmd_mkdir},
    {"rm",    cmd_rm},
    {"cp",    cmd_cp},
    {"mv",    cmd_mv},
    {"touch", cmd_touch},
    {"chmod", cmd_chmod},
    {"whoami",cmd_whoami},
    {"uname", cmd_uname},
    {"clear", cmd_clear},
    {"help",  cmd_help},
    {"exit",  cmd_exit},
    {"sudo",  cmd_sudo},
    {NULL,    NULL}
};

/* ============================================
 * 执行内置命令
 * ============================================ */

int execute_builtin(char **argv, int argc)
{
    if (!argv || !argv[0]) {
        return 1;
    }

    for (int i = 0; builtins[i].name; i++) {
        if (str_cmp(argv[0], builtins[i].name) == 0) {
            return builtins[i].func(argv, argc);
        }
    }

    return -1;  /* 不是内置命令 */
}

/* ============================================
 * 执行外部程序 (使用 fork+exec)
 * ============================================ */

int execute_external(char **argv, int argc)
{
    (void)argc;
    char path[MAX_PATH];

    /* 如果是绝对路径，直接使用 */
    if (argv[0][0] == '/') {
        str_ncpy(path, argv[0], MAX_PATH - 1);
    } else {
        /* 在 PATH 中查找 */
        str_cpy(path, "/bin/");
        str_cat(path, argv[0]);
    }

    char *envp[] = {
        "PATH=/bin:/usr/bin:/prgm/sys:/prgm",
        "HOME=/root",
        "TERM=linux",
        NULL
    };

    /* 使用 fork+exec 执行外部程序 */
    long pid = sys_fork();
    if (pid < 0) {
        print(argv[0]);
        println(": fork failed");
        return 1;
    }

    if (pid == 0) {
        /* 子进程: 尝试执行程序 */

        /* 尝试绝对路径或 /bin/ */
        long ret = sys_execve(path, argv, envp);

        /* 尝试 /prgm/sys/ */
        if (ret < 0) {
            str_cpy(path, "/prgm/sys/");
            str_cat(path, argv[0]);
            ret = sys_execve(path, argv, envp);
        }

        /* 尝试 /usr/bin/ */
        if (ret < 0) {
            str_cpy(path, "/usr/bin/");
            str_cat(path, argv[0]);
            ret = sys_execve(path, argv, envp);
        }

        /* 尝试 /prgm/ */
        if (ret < 0) {
            str_cpy(path, "/prgm/");
            str_cat(path, argv[0]);
            ret = sys_execve(path, argv, envp);
        }

        /* 所有路径都失败 */
        print(argv[0]);
        println(": Command not found");
        sys_exit(127);
    }

    /* 父进程: 等待子进程完成 */
    int status = 0;
    sys_wait(-1, &status, 0, NULL);
    return (status >> 8) & 0xFF;
}
