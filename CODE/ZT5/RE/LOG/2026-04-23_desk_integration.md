# 2026-04-23 Desk 桌面环境架构重构

## 概述

将 `zrrWindowsManager`（窗口管理器库）和 `zrrExfileManager`（文件管理器库）集成到 `desk.c` 单文件中，简化构建系统，减少二进制大小，提高代码可维护性。

## 变更背景

### 旧架构问题

1. **zrrWindowsManager** (~50KB)
   - `wm_composite()` 函数被注释掉（标注"避免浮点操作崩溃"）
   - 窗口根本无法渲染
   - 作为独立库存在，增加了构建复杂度

2. **zrrExfileManager** (~40KB)
   - `exfile_render()` 函数是空壳，只声明未实现
   - 功能不完整，无法独立使用

3. **构建系统复杂**
   - 需要分别编译三个组件 (desk + 2 managers)
   - limine.conf 需要配置三个模块路径
   - zratro.conf 需要启动三个独立进程

### 新架构优势

1. **单文件集成** - 所有功能在 `desk.c` 中实现
2. **二进制优化** - 从 ~90KB 减少到 ~22KB
3. **构建简化** - 只需编译一个目标文件
4. **启动优化** - 只需启动一个进程

## 重构内容

### 一、代码集成

#### 1.1 窗口管理器功能 (来自 zrrWindowsManager)

集成到 `desk.c` 的核心功能：

| 功能 | 实现 |
|------|------|
| 窗口 CRUD | `win_create()`, `win_destroy()`, `win_find_by_id()` |
| 焦点管理 | `win_set_focus()`, `win_blur()` |
| Z-order 排序 | `win_sort_z_order()` |
| 窗口状态 | `WIN_VISIBLE`, `WIN_MINIMIZED`, `WIN_MAXIMIZED` |
| 窗口操作 | `win_minimize()`, `win_maximize()`, `win_restore()`, `win_close()` |
| 命中测试 | `win_hit_test()` |
| 窗口绘制 | `draw_window()` - 阴影、圆角、标题栏、按钮 |

#### 1.2 文件管理器功能 (来自 zrrExfileManager)

集成到 `desk.c` 的核心功能：

| 功能 | 实现 |
|------|------|
| 目录浏览 | `fm_open()`, `fm_refresh()` |
| 图标视图 | `fm_draw_icons()` |
| 列表视图 | `fm_draw_list()` |
| 目录导航 | `fm_navigate()`, `fm_go_up()`, `fm_go_back()`, `fm_go_forward()` |
| 地址栏 | `fm_draw_address_bar()` |
| 侧边栏 | `fm_draw_sidebar()` (快速访问) |
| 双击打开 | 在 `handle_mouse_click()` 中处理 |

#### 1.3 任务栏功能

| 功能 | 实现 |
|------|------|
| 开始按钮 | 点击弹出开始菜单 |
| 窗口按钮 | 显示活动窗口，点击切换焦点/最小化 |
| 时钟 | 显示当前时间 (小时:分钟) |

#### 1.4 开始菜单

| 功能 | 实现 |
|------|------|
| 应用列表 | `g_start_items[]` 数组 |
| 弹出动画 | 浮出效果 |
| 点击处理 | 启动对应应用 |

### 二、构建系统更新

#### 2.1 Makefile 变更

**文件**: `SYSTEM/lib/desk/Makefile`

**变更前**:
```makefile
CFLAGS = ... -I../zrrWindowsManager -I../zrrExfileManager
LDFLAGS = ... --allow-multiple-definition
```

**变更后**:
```makefile
CFLAGS = -ffreestanding -fno-stack-protector -fno-pic -fno-pie \
         -fno-builtin -nostdlib -nostdinc \
         -mno-red-zone -mno-mmx -mno-sse -mno-sse2 \
         -mcmodel=small -m64 \
         -Wall -Wextra -Wno-unused-function -Wno-unused-variable \
         -O2 -pipe \
         -mgeneral-regs-only -fno-tree-vectorize

LDFLAGS = -static -nostdlib -T desk.ld
```

**关键变更**:
- 移除对 `zrrWindowsManager` 和 `zrrExfileManager` 头文件路径的引用
- 添加 `-Wno-unused-function -Wno-unused-variable` 抑制警告
- 移除 `--allow-multiple-definition` 链接标志
- 添加 `-mno-sse -mno-sse2 -mgeneral-regs-only -fno-tree-vectorize` 确保无浮点运算

#### 2.2 limine.conf 变更

**变更前**:
```
module_path: boot():/prgm/sys/desk
module_path: boot():/system/zrdemage/zrrWindowsManager
module_path: boot():/system/zrdemage/zrrExfileManager
```

**变更后**:
```
module_path: boot():/prgm/sys/desk
# zrrWindowsManager and zrrExfileManager now integrated into desk
```

#### 2.3 zratro.conf 变更

**变更前**:
```
program = /prgm/sys/desk
program = /system/zrdemage/zrrWindowsManager
program = /system/zrdemage/zrrExfileManager
```

**变更后**:
```
program = /prgm/sys/desk
# zrrWindowsManager and zrrExfileManager are now integrated into desk
```

### 三、文件清理

#### 3.1 删除的目录

- `SYSTEM/lib/zrrWindowsManager/` - 窗口管理器库源码
- `SYSTEM/lib/zrrExfileManager/` - 文件管理器库源码
- `SYSTEM/system/zrdemage/` - 旧管理器二进制目录

#### 3.2 保留的文件

- `SYSTEM/lib/desk/desk.c` - 集成后的桌面环境
- `SYSTEM/lib/desk/Makefile` - 更新后的构建脚本
- `SYSTEM/lib/desk/desk.ld` - 链接脚本（不变）

### 四、desk.c 架构

#### 4.1 核心数据结构

```c
/* 窗口状态 */
typedef enum {
    WIN_INVISIBLE,
    WIN_VISIBLE,
    WIN_MINIMIZED,
    WIN_MAXIMIZED
} win_state_t;

/* 窗口标志 */
#define WIN_FLAG_TITLEBAR   (1 << 0)
#define WIN_FLAG_CLOSE      (1 << 1)
#define WIN_FLAG_MAX        (1 << 2)
#define WIN_FLAG_MIN        (1 << 3)
#define WIN_FLAG_BORDER     (1 << 4)
#define WIN_FLAG_SHADOW     (1 << 5)

/* 窗口结构 */
struct desk_window {
    uint32_t id;
    char title[64];
    int x, y, w, h;
    int content_w, content_h;
    uint32_t *content_fb;
    win_state_t state;
    uint32_t flags;
    uint32_t z_order;
    uint32_t bg_color;
    uint32_t titlebar_color;
    int has_focus;
    /* 回调函数 */
    void (*on_draw)(struct desk_window *);
    void (*on_click)(struct desk_window *, int, int, int);
};

/* 全局状态 */
struct {
    uint32_t *fb;
    int w, h, stride;
    struct desk_window windows[MAX_WINDOWS];
    int next_win_id;
    int max_z_order;
    struct {
        int x, y;
        int left, right, middle;
        int prev_left;
        int dragging;
        uint32_t drag_win_id;
        int drag_ox, drag_oy;
    } mouse;
    struct {
        int height;
        int start_menu_open;
        int clock_hour, clock_min;
        struct { uint32_t win_id; } win_btns[MAX_WINDOWS];
        int win_btn_count;
    } taskbar;
} g_desk;
```

#### 4.2 渲染流水线

```
composite_all()
    ├── draw_background()        # 渐变背景 + 网格
    ├── draw_window() x N        # 按 z-order 绘制窗口
    ├── draw_taskbar()           # 底部任务栏
    ├── draw_start_menu()        # 开始菜单（如果打开）
    ├── draw_cursor()            # 鼠标光标
    └── sys_fb_sync()            # 同步到 framebuffer
```

#### 4.3 事件处理流水线

```
主循环
    ├── 读取鼠标状态
    ├── handle_mouse_move()      # 更新鼠标位置，处理拖拽
    ├── handle_mouse_click()     # 处理点击事件
    │   ├── 开始菜单区域 → 启动应用
    │   ├── 任务栏区域 → 切换窗口
    │   ├── 窗口命中测试 → 设置焦点/拖拽/按钮
    │   └── 桌面区域 → 取消选择
    └── composite_all()          # 渲染并显示
```

### 五、编译验证

```bash
cd SYSTEM/lib/desk
make clean && make
# 输出: SYSTEM/prgm/sys/desk (22KB ELF64)
```

**编译标志验证**:
- ✅ 无 SSE/浮点指令 (`-mno-sse -mno-sse2 -mgeneral-regs-only`)
- ✅ 无向量化的代码 (`-fno-tree-vectorize`)
- ✅ 入口点正确 (`_start` at `0x400000`)
- ✅ Framebuffer 符号正确 (`fb_base`, `fb_width`, `fb_height` at `0x401000`)

### 六、二进制验证

```bash
readelf -h SYSTEM/prgm/sys/desk
# ELF 64-bit, x86-64, Entry: 0x400000

nm SYSTEM/prgm/sys/desk | grep -E "fb_|logo_"
# 0000000000401000 A fb_pitch
# 0000000000401004 A fb_height
# 0000000000401008 A fb_width
# 000000000040100c A fb_base
```

## 影响范围

### 更新的文档

- `README.md` - 更新版本号和架构描述
- `RE/DEVELOPMENT_GUIDE.md` - 更新目录结构和用户空间描述
- `RE/KERNEL_IMPLEMENTATION_DETAILS.md` - 更新版本号
- `RE/LINUX_TO_ZTS_MIGRATION.md` - 更新日期
- `RE/LOG/2026-04-19_desk_gui_event_loop.md` - 添加后续重构说明

### 更新的文件

| 文件 | 变更 |
|------|------|
| `SYSTEM/lib/desk/desk.c` | 重写为集成架构 |
| `SYSTEM/lib/desk/Makefile` | 简化编译配置 |
| `SYSTEM/boot/limine.conf` | 移除旧管理器模块 |
| `SYSTEM/system/zratro.conf` | 移除旧管理器启动项 |

### 删除的文件

| 文件 | 说明 |
|------|------|
| `SYSTEM/lib/zrrWindowsManager/` | 窗口管理器库源码 |
| `SYSTEM/lib/zrrExfileManager/` | 文件管理器库源码 |
| `SYSTEM/system/zrdemage/zrrWindowsManager` | 旧二进制 |
| `SYSTEM/system/zrdemage/zrrExfileManager` | 旧二进制 |

## 技术要点

1. **单文件集成** - 1557 行代码包含完整桌面环境
2. **全整数运算** - 避免浮点/SSE 导致的崩溃
3. **直接 framebuffer 渲染** - 无合成器开销
4. **事件驱动架构** - 鼠标点击/移动实时响应
5. **模块化设计** - 窗口、文件管理器、任务栏独立但集成

## 后续工作

1. **键盘输入** - 添加键盘事件处理
2. **应用启动** - 完善开始菜单的应用启动功能
3. **文件管理器完善** - 实现文件双击打开、复制粘贴等功能
4. **图标渲染** - 从文件系统加载应用图标
5. **窗口内容** - 支持应用程序渲染到窗口 framebuffer

## 状态

- [x] 窗口管理器集成
- [x] 文件管理器集成
- [x] 任务栏实现
- [x] 开始菜单实现
- [x] 鼠标事件处理
- [x] 窗口拖拽
- [x] 窗口最小化/最大化/还原/关闭
- [x] 文件管理器目录浏览
- [x] 构建系统更新
- [x] limine.conf 更新
- [x] zratro.conf 更新
- [x] 旧文件清理
- [x] 文档更新
- [x] ISO 重建
- [ ] QEMU 完整测试验证
