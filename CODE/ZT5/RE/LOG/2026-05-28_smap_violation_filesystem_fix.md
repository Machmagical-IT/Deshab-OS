# 2026-05-28 文件系统 SMAP 违规全面修复 & 登录系统排查

## 概述

用户报告桌面无法登录，`/ini/usrroot/user.cfg` 无法解析。经深入排查发现根本原因是 **SMAP（Supervisor Mode Access Prevention）违规**——系统调用层直接将用户空间指针传递给内核函数，在 SMAP 启用时触发 #PF 页错误，导致所有文件 I/O 返回失败。同时发现 FAT32 文件系统多处逻辑缺陷和桌面配置解析错误。

## 问题层级

### 🔴 致命错误（Fatal）

#### F1: sys_open SMAP 违规——所有文件打开失败

- **文件**: `SYSTEM/lib/Ztknl/syscall.c` `sys_open()`
- **根因**: `vfs_open(path, vfs_mode)` 直接传递用户空间 `path` 指针。`vfs_open` 内部 `strlen(path)`、`find_mount(path)` 直接解引用用户空间指针。SMAP 启用时触发 #PF，`sys_open` 返回 -1。
- **触发**: 所有 `SYS_OPEN` 调用，SMAP 启用环境 100% 触发
- **修复**: `copy_string_from_user(kpath, path, sizeof(kpath))` 后 `vfs_open(kpath, vfs_mode)`

#### F2: sys_read/sys_write SMAP 违规——所有文件读写失败

- **文件**: `SYSTEM/lib/Ztknl/syscall.c` `sys_read()` / `sys_write()`
- **根因**: `fat32_read` 内部 `memcpy((uint8_t *)buf + total_read, ...)` 直接写入用户空间缓冲区；`fat32_write` 同理直接读取用户空间缓冲区
- **触发**: 所有文件读写操作，SMAP 启用时 100% 触发
- **修复**:
  - `sys_read`: 4KB 内核缓冲区 `kbuf` 中转，`vfs_read(file, kbuf, to_read)` 后 `copy_to_user(buf, kbuf, ret)`
  - `sys_write`: `copy_from_user(kbuf, buf, chunk)` 后 `vfs_write(file, kbuf, chunk)`

#### F3: sys_stat SMAP 违规——文件信息获取失败

- **文件**: `SYSTEM/lib/Ztknl/syscall.c` `sys_stat()`
- **根因**: `vfs_stat(path, &kst)` 直接传递用户空间 `path` 指针
- **修复**: `copy_string_from_user(kpath, path, sizeof(kpath))` 后 `vfs_stat(kpath, &kst)`

#### F4: sys_readdir SMAP 违规——目录读取失败

- **文件**: `SYSTEM/lib/Ztknl/syscall.c` `sys_readdir()`
- **根因**: `user_dirent->name[i] = kern_dirent.name[i]` 直接写入用户空间
- **修复**: 内核 `struct fm_dirent kern_fm` 中转，`copy_to_user(dirp, &kern_fm, sizeof(kern_fm))`

#### F5: sys_sudo SMAP 违规——密码验证失败

- **文件**: `SYSTEM/lib/Ztknl/syscall.c` `sys_sudo()`
- **根因**: `password_hash[i] != root_hash[i]` 直接读取用户空间内存
- **修复**: `copy_string_from_user(khash, password_hash, sizeof(khash))` 后使用 `khash[i]`

### 🟠 严重缺陷（Severe）

#### S1: FAT32 LFN 短文件名索引计算错误

- **文件**: `SYSTEM/lib/Ztknl/fat32.c` `parse_path_attr()`
- **根因**: LFN 条目后短文件名索引应为 `j + num_lfn`，而非 `j + 1`。`j + 1` 指向第二个 LFN 条目，导致 `file_cluster` 为 0
- **修复**: `short_idx = j + num_lfn`，增加跨簇边界处理

#### S2: FAT32 FAT 表缓存脏数据丢失

- **文件**: `SYSTEM/lib/Ztknl/fat32.c` `fat32_read_fat_entry()`
- **根因**: 切换缓存扇区时未检查 `fat_buffer_dirty`，直接覆盖导致写入数据丢失
- **修复**: 切换前检查脏标志，若脏则 `fat32_flush_fat_cache_internal()`

#### S3: desk.c USER_CFG_PATH 缺少前导 `/`

- **文件**: `SYSTEM/lib/desk/desk.c` `USER_CFG_PATH` 宏
- **根因**: `"ini/usrroot/user.cfg"` 为相对路径，VFS `find_mount` 只匹配绝对路径
- **修复**: 修改为 `"/ini/usrroot/user.cfg"`

#### S4: desk.c parse_user_cfg 用户块分隔逻辑错误

- **文件**: `SYSTEM/lib/desk/desk.c` `parse_user_cfg()`
- **根因**: `user.cfg` 格式中 `uid` 在 `name` 之前，但解析器仅在 `name` 键时触发新用户块分隔。第二个用户块的 `uid=1000` 被写入第一个用户结构体
- **修复**: `uid` 键处理中也添加新用户块分隔逻辑

#### S5: fat32_open 空文件（cluster=0）无法打开

- **文件**: `SYSTEM/lib/Ztknl/fat32.c` `fat32_open()`
- **根因**: `cluster == 0`（空文件）时跳过文件大小搜索，`file_exists` 保持 0
- **修复**: 移除 `cluster != 0` 前置条件，`cluster == 0` 时通过文件名匹配确认存在

### 🟡 一般问题（General）

#### G1: sys_getcwd SMAP 违规

- **文件**: `SYSTEM/lib/Ztknl/syscall.c` `sys_getcwd()`
- **根因**: `buf[0] = '/'` 直接写入用户空间
- **修复**: `copy_to_user(buf, cwd, len)`

#### G2: sys_chmod/sys_chown SMAP 违规

- **文件**: `SYSTEM/lib/Ztknl/syscall.c`
- **根因**: `vfs_chmod(path, mode)` / `vfs_chown(path, uid, gid)` 直接传递用户空间 `path`
- **影响**: FAT32 不支持 chmod/chown，暂无实际影响

#### G3: sys_exec SMAP 违规

- **文件**: `SYSTEM/lib/Ztknl/syscall.c` `sys_exec()`
- **根因**: `elf_load_file(path, ...)` / `build_process_image(path, ...)` 直接使用用户空间指针
- **影响**: 所有程序执行，需深入 ELF 加载器修复

#### G4: pipe_read/pipe_write SMAP 违规

- **文件**: `SYSTEM/lib/Ztknl/ipc.c`
- **根因**: `dst[i] = pipe->buffer[...]` 直接写入用户空间
- **影响**: Shell 管道操作

#### G5: desk.c BMP 加载未使用 data_off

- **文件**: `SYSTEM/lib/desk/desk.c` `load_bg_image()`
- **根因**: BMP 像素数据可能从 `data_off` 偏移开始（>54），直接从 54 字节后读取导致数据错位
- **修复**: 读取前跳过 `data_off - 54` 字节

### 🟢 潜在风险（Potential）

#### P1: ISR 入口未调用 stac/clac

- **文件**: `SYSTEM/lib/Ztknl/idt_asm.S` `isr_common_stub`
- **说明**: 系统调用入口未调用 `stac` 指令。当前通过逐个系统调用修复 SMAP 问题，但遗漏的系统调用可能仍有问题。建议在 ISR 入口统一添加 `stac/clac` 作为安全网

#### P2: vfs_permission_check 每次读取都重新解析路径

- **文件**: `SYSTEM/lib/Ztknl/vfs.c` `vfs_read()`
- **说明**: 每次文件读取调用 `vfs_permission_check` → `vfs_stat` → `fat32_stat` → `parse_path_attr`，重新从磁盘解析整个路径。严重性能退化

#### P3: sys_read 内核缓冲区仅 4096 字节

- **文件**: `SYSTEM/lib/Ztknl/syscall.c` `sys_read()`
- **说明**: 为修复 SMAP 引入的 4KB 内核缓冲区，大文件读取需要多次系统调用

## SMAP 违规排查方法论

### 关键发现

`idt_asm.S` 中 `isr_common_stub` 和 `syscall_isr` 均未包含 `stac` 指令。这意味着系统调用处理期间 SMAP 始终生效。任何内核态对用户空间的直接访问（包括 `strlen()`、`memcpy()`、指针解引用）都会触发 #PF。

### 检查模式

```c
// ❌ 危险：直接使用用户空间指针
vfs_open(path, mode);           // path 来自用户空间
vfs_read(file, buf, count);     // buf 是用户空间缓冲区
memcpy(buf, src, len);          // buf 是用户空间指针

// ✅ 安全：通过内核缓冲区中转
copy_string_from_user(kpath, path, sizeof(kpath));
vfs_open(kpath, mode);

char kbuf[4096];
vfs_read(file, kbuf, to_read);
copy_to_user(buf, kbuf, ret);
```

### 需要继续排查的系统调用

| 系统调用 | 状态 | 说明 |
|----------|------|------|
| sys_exec | ⚠️ 未修复 | `elf_load_file(path, ...)` 直接使用用户空间指针 |
| sys_execve | ⚠️ 未修复 | 同上，且 argv/envp 数组也需要安全复制 |
| sys_chmod | ⚠️ 未修复 | `vfs_chmod(path, mode)` 直接传递用户空间指针 |
| sys_chown | ⚠️ 未修复 | `vfs_chown(path, uid, gid)` 直接传递用户空间指针 |
| pipe_read | ⚠️ 未修复 | 直接写入用户空间缓冲区 |
| pipe_write | ⚠️ 未修复 | 直接读取用户空间缓冲区 |

## 修改文件清单

| 文件 | 修改内容 |
|------|----------|
| `SYSTEM/lib/Ztknl/syscall.c` | 修复 sys_open/sys_read/sys_write/sys_stat/sys_readdir/sys_sudo/sys_getcwd SMAP 违规；清理 sys_open 冗余检查 |
| `SYSTEM/lib/Ztknl/fat32.c` | 修复 LFN 短文件名索引计算、FAT 缓存脏数据丢失、空文件无法打开 |
| `SYSTEM/lib/desk/desk.c` | 修复 USER_CFG_PATH 路径、SYS_READDIR 调用参数、BMP data_off、parse_user_cfg 用户块分隔逻辑 |

## 调用链追踪

```
desk.c: parse_user_cfg()
  → SYS_OPEN("/ini/usrroot/user.cfg", O_RDONLY)
    → sys_open() [SMAP 违规已修复: kpath 中转]
      → vfs_open(kpath, vfs_mode)
        → find_mount(kpath) → get_rel_path(kpath, mount)
        → fat32_open(file, rel_path, mode)
          → parse_path(rel_path, clusters, 32)
            → parse_path_attr(rel_path, clusters, NULL, NULL, 32)
              → compare_filename(component, entry_name) [LFN 索引已修复]
  → SYS_READ(fd, buf, 1023)
    → sys_read() [SMAP 违规已修复: kbuf 中转]
      → vfs_read(file, kbuf, to_read)
        → vfs_permission_check(file->path, S_IRUSR)
        → fat32_read(file, kbuf, to_read)
      → copy_to_user(buf, kbuf, ret)
  → SYS_CLOSE(fd)
```

## 后续优化方向

1. **ISR 入口统一 stac/clac**: 在 `isr_common_stub` 入口添加 `stac`，出口添加 `clac`，作为全局安全网
2. **缓存 vfs_stat 结果**: 在 `vfs_file` 结构体中缓存 stat 结果，避免每次 read/write 重新解析路径
3. **移除 fat32_open 冗余搜索**: `parse_path_attr` 已返回 `file_size`，`fat32_open` 不需要再次搜索父目录
4. **修复剩余 SMAP 违规**: sys_exec、pipe_read/write 等
5. **动态内核缓冲区**: 替换 sys_read 中固定 4KB 缓冲区为动态分配

---

## 第二轮修复：ATA 磁盘写入致命 Bug（2026-05-28 续）

### 问题现象

QEMU 测试发现：
- 内核启动时 `fat32_open("ini/usrroot/user.cfg")` 成功（depth=4）
- desk 程序启动后 `fat32_open("ini/usrroot/user.cfg")` 失败（depth=-1）
- 所有 `mkdir` 子目录创建失败（Parent directory not found）
- `parse_path` 找不到刚创建的目录

### 根因分析

通过 QEMU 串口日志追踪，发现：

1. `mkdir /ini` 成功写入 LBA 3184（根目录簇）
2. 紧接着 `parse_path("/ini")` 读取 LBA 3184，但找不到 `ini` 条目
3. **写入磁盘的数据没有被正确持久化！**

检查 `disk_write_sector()` 发现：

#### 🔴 F6: ATA 写入使用 DMA 命令但 PIO 数据传输

- **文件**: `SYSTEM/lib/Ztknl/disk.c` `disk_write_sector()`
- **根因**: 使用 `ATA_CMD_WRITE_DMA_EXT` (0x35) 命令，但通过 `outw(ATA_DATA, data[i])` PIO 方式传输数据。DMA 写入需要设置 PRD（Physical Region Descriptor）表和 BM DMA 寄存器，不能直接用 PIO 端口写入数据。QEMU 收到 DMA 命令后不处理 PIO 数据传输，导致数据实际未写入磁盘。
- **影响**: 所有磁盘写入操作静默失败——`mkdir` 创建的目录条目未写入、FAT 表更新丢失、文件内容写入无效
- **修复**: 改用 `ATA_CMD_WRITE_PIO_EXT` (0x34) PIO 写入命令

#### 🟠 S5: ATA 读取重试始终使用 LBA48 模式

- **文件**: `SYSTEM/lib/Ztknl/disk.c` `disk_read_sector()`
- **根因**: 读取超时重试时，无论原始请求是 LBA28 还是 LBA48，都使用 LBA48 模式重新发送命令。虽然对 LBA < 0x0FFFFFFF 的地址 LBA48 也能工作，但与原始命令不一致，可能在某些硬件上导致问题
- **修复**: 根据 LBA 大小选择 LBA28 或 LBA48 重试命令

#### 🟠 S6: ATA 超时使用 TSC 计数器在 QEMU 中不准确

- **文件**: `SYSTEM/lib/Ztknl/disk.c` `ata_wait_ready()` / `ata_wait_data()`
- **根因**: 使用 `rdtsc()` 计算超时，但 QEMU 中 TSC 频率与实际 CPU 频率不匹配，导致超时值不准确。5 秒超时在 QEMU 中可能只有几百毫秒，PIO 写入后等待 BSY 清除时误判超时
- **影响**: PIO 写入后 `ata_wait_ready()` 误判超时，导致后续 `mkdir` 分配簇失败
- **修复**: 改用循环计数器（5000 万次迭代），不依赖 TSC

#### 🟡 G1: PIO 写入后缺少 FLUSH CACHE 命令

- **文件**: `SYSTEM/lib/Ztknl/disk.c` `disk_write_sector()`
- **根因**: PIO 写入完成后未发送 `FLUSH CACHE EXT` (0xEA) 命令，数据可能停留在磁盘写缓存中，未写入物理介质。在 QEMU 中这不是问题（写入是同步的），但在真实硬件上可能导致断电数据丢失
- **修复**: 写入完成后发送 `FLUSH CACHE EXT` 命令并等待完成

### 修复代码

```c
// disk.c - 修复1: DMA → PIO 写入命令
// 修复前:
outb(ATA_COMMAND, 0x35);  /* ATA_CMD_WRITE_DMA_EXT */
// 修复后:
outb(ATA_COMMAND, 0x34);  /* ATA_CMD_WRITE_PIO_EXT */

// disk.c - 修复2: TSC 超时 → 循环计数
// 修复前:
static int ata_wait_ready(void) {
    uint64_t timeout = 5000000000ULL;
    uint64_t start = rdtsc();
    while (1) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_STATUS_BSY)) return 0;
        if ((rdtsc() - start) > timeout) return -1;
        for (volatile int i = 0; i < 100; i++);
    }
}
// 修复后:
static int ata_wait_ready(void) {
    for (volatile int timeout = 0; timeout < 50000000; timeout++) {
        uint8_t status = inb(ATA_STATUS);
        if (!(status & ATA_STATUS_BSY)) return 0;
    }
    return -1;
}

// disk.c - 修复3: 添加 FLUSH CACHE
// 在 disk_write_sector() 末尾添加:
outb(ATA_COMMAND, 0xEA);  /* FLUSH CACHE EXT */
ata_wait_ready();

// disk.c - 修复4: 读取重试命令模式匹配
// 修复前: 重试始终使用 LBA48
// 修复后: 根据 lba 大小选择 LBA28/LBA48
if (lba < 0x0FFFFFFF) {
    outb(ATA_DRIVE, 0xE0 | ((lba >> 24) & 0x0F));
    outb(ATA_SECTOR_COUNT, 1);
    outb(ATA_LBA_LOW, lba & 0xFF);
    outb(ATA_LBA_MID, (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
    outb(ATA_COMMAND, ATA_CMD_READ_PIO);
} else { /* LBA48 */ }
```

### 验证结果

QEMU 测试通过：
- 所有 17 个目录创建成功（之前只有 7 个成功，10 个失败）
- `user.cfg` 成功读取 160 字节并解析出 root 和 user 两个用户
- 登录界面正常显示
- AUTOFS 所有文件创建成功，无 FAIL

### 问题影响链

```
disk_write_sector 使用 DMA 命令 (0x35)
  → QEMU 不处理 PIO 数据传输
    → 写入数据实际未落盘
      → mkdir 创建的目录条目丢失
        → parse_path 找不到新目录
          → 子目录 mkdir 全部失败 (Parent directory not found)
            → desk 程序无法打开 /ini/usrroot/user.cfg
              → 解析出 0 个用户
                → 登录功能完全失效
```
