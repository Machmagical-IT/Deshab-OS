# 2026-04-19 Desk GUI 事件循环修复

## 概述

修复 desk 程序启动后无响应的问题，实现了完整的事件循环和桌面渲染。

## 问题描述

desk 程序加载成功后，串口日志显示：
```
[BOOT] Loading: /prgm/sys/desk
[LOAD] VFS OK, proc=0xFFFFFFFF81018720
[LOAD] Setting framebuffer info for desk
[LOAD] Logo size OK, passing to desk
[SCHED] pick_next thread found
```

但屏幕只显示静态背景，桌面不响应任何操作。

## 根因分析

阅读 `SYSTEM/lib/desk/desk.c` 发现：

1. `_start` 函数只执行一次渲染，然后进入空循环
2. **关键问题**：没有主事件循环，无法响应用户输入

## 修复方案

修改 `_start` 函数，添加完整的事件循环：

```c
/* 主事件循环 */
while (g.running) {
    /* 清屏 -> 背景 -> 图标 -> 窗口 -> 任务栏 -> 菜单 -> 光标 */
    /* framebuffer 同步 */
}
```

## 技术细节

### 渲染顺序（从后到前）
1. 清屏
2. 渐变背景（C_BG_DARKEST -> C_BG_DARK）
3. 桌面图标（Terminal, Files, Monitor, Settings）
4. 窗口（按 z-order）
5. 任务栏（底部，带开始按钮、窗口按钮、时钟）
6. 开始菜单（点击开始按钮弹出）
7. 右键菜单（桌面右键点击弹出）
8. 通知（右上角弹出）
9. 鼠标光标（最顶层）

## 影响文件

- `SYSTEM/lib/desk/desk.c` - 修改 _start 函数，添加事件循环
- `ISO/zratroos.img` - 重新构建

## 状态

- [x] desk 事件循环实现
- [x] 桌面背景渲染
- [x] 桌面图标显示
- [x] 窗口系统接入
- [x] 任务栏渲染
- [x] 开始菜单/右键菜单
- [x] 通知系统
- [x] 鼠标光标显示

---

**注意**: 此版本为早期实现，后续在 2026-04-23 进行了架构重构：
- 将 `zrrWindowsManager` 和 `zrrExfileManager` 集成到 `desk.c`
- 实现了完整的窗口管理器、文件管理器、任务栏和输入处理
- 二进制大小从 ~53KB 优化到 ~22KB
- 详见 `RE/LOG/2026-04-23_desk_integration.md`
