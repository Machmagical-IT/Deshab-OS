#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>
#include <stddef.h>
#include "limine.h"

/* ═══════════════════════════════════════════════════════════════════════════
 * ZRATRO CONSOLE SYSTEM - 独特控制台输出风格
 * ═══════════════════════════════════════════════════════════════════════════ */

/*─── Zratro 独特颜色定义 ───*/
#define Z_COLOR_BLACK           0x00000000U
#define Z_COLOR_WHITE           0x00FFFFFFU
#define Z_COLOR_RED             0x00FF3333U     /* Zratro 红 */
#define Z_COLOR_GREEN           0x0033FF33U     /* Zratro 绿 */
#define Z_COLOR_BLUE            0x003399FFU     /* Zratro 蓝 */
#define Z_COLOR_YELLOW          0x00FFFF33U     /* Zratro 黄 */
#define Z_COLOR_CYAN            0x0000FFFFU     /* Zratro 青 */
#define Z_COLOR_MAGENTA         0x00FF33FFU     /* Zratro 品红 */
#define Z_COLOR_ORANGE          0x00FF9933U     /* Zratro 橙 */
#define Z_COLOR_PURPLE          0x009933FFU     /* Zratro 紫 */
#define Z_COLOR_GRAY            0x00888888U     /* Zratro 灰 */
#define Z_COLOR_DARK_GRAY       0x00444444U     /* Zratro 深灰 */
#define Z_COLOR_AURORA_GREEN    0x0000FF88U     /* 极光绿 - 品牌色 */
#define Z_COLOR_AURORA_BLUE     0x000088FFU     /* 极光蓝 - 品牌色 */

/*─── 控制台 UI 颜色 (GitHub Dark 风格) ───*/
#define CONSOLE_BG_DARK         0x0D1117U       /* GitHub 风格深色背景 */
#define CONSOLE_BG_BOOT         0x000000U       /* 启动画面背景 - 纯黑色 */
#define CONSOLE_TEXT_WHITE      0xF0F6FCU       /* 白色文字 */
#define CONSOLE_TEXT_GRAY       0x8B949EU       /* 灰色文字 */
#define CONSOLE_ERROR_RED       0xFF4757U       /* 错误/红色 */
#define CONSOLE_WARNING_YELLOW  0xFFB800U       /* 警告/黄色 */
#define CONSOLE_INFO_CYAN       0x00D4FFU       /* 信息/青色 */
#define CONSOLE_SUCCESS_GREEN   0x00FF88U       /* 成功/绿色 */
#define CONSOLE_HIGHLIGHT_BLUE  0x1E88E5U       /* 高亮蓝 */
#define CONSOLE_HIGHLIGHT_ORG   0xFB8C00U       /* 高亮橙 */

/*─── 便捷颜色设置宏 ───*/
#define CONSOLE_SET_DEFAULT()   console_set_color(CONSOLE_TEXT_WHITE, CONSOLE_BG_DARK)
#define CONSOLE_SET_ERROR()     console_set_color(CONSOLE_ERROR_RED, CONSOLE_BG_DARK)
#define CONSOLE_SET_WARNING()   console_set_color(CONSOLE_WARNING_YELLOW, CONSOLE_BG_DARK)
#define CONSOLE_SET_INFO()      console_set_color(CONSOLE_INFO_CYAN, CONSOLE_BG_DARK)
#define CONSOLE_SET_SUCCESS()   console_set_color(CONSOLE_SUCCESS_GREEN, CONSOLE_BG_DARK)
#define CONSOLE_SET_GRAY()      console_set_color(CONSOLE_TEXT_GRAY, CONSOLE_BG_DARK)

/*─── Zratro 独特日志图标 ───*/
#define Z_ICON_SUCCESS          "✓"     /* UTF-8 成功图标 */
#define Z_ICON_ERROR            "✗"     /* UTF-8 错误图标 */
#define Z_ICON_WARNING          "⚠"     /* UTF-8 警告图标 */
#define Z_ICON_INFO             "ℹ"     /* UTF-8 信息图标 */
#define Z_ICON_DEBUG            "◇"     /* UTF-8 调试图标 */
#define Z_ICON_ARROW            "→"     /* UTF-8 箭头 */
#define Z_ICON_BULLET           "●"     /* UTF-8 项目符号 */
#define Z_ICON_STAR             "★"     /* UTF-8 星号 */
#define Z_ICON_GEAR             "⚙"     /* UTF-8 齿轮 */
#define Z_ICON_LOCK             "🔒"    /* UTF-8 锁 */
#define Z_ICON_BUG              "🐛"    /* UTF-8 Bug */

/*─── ASCII 替代图标 (兼容模式) ───*/
#define Z_ASCII_SUCCESS         "[OK]"
#define Z_ASCII_ERROR           "[XX]"
#define Z_ASCII_WARNING         "[!!]"
#define Z_ASCII_INFO            "[..]"
#define Z_ASCII_DEBUG           "[--]"
#define Z_ASCII_ARROW           "->"
#define Z_ASCII_BULLET          "*"
#define Z_ASCII_STAR            "*"
#define Z_ASCII_GEAR            "#"

/* 初始化控制台 */
void console_init(void);

/* 初始化控制台 (带帧缓冲请求) */
void console_init_with_fb(volatile struct limine_framebuffer_request *fb_req);

/* 打印字符串 */
void print(const char *str);

/* 打印字符串并换行 */
void println(const char *str);

/* 打印字符 */
void putchar(char c);

/* 打印十六进制数 */
void print_hex(uint64_t value);

/* 打印十进制数 */
void print_dec(int64_t value);

/* printk 风格格式化打印 (Linux 内核风格) */
void printk(const char *fmt, ...);

/* 清屏 */
void clear_screen(void);

/* 重置光标位置 */
void console_reset_cursor(void);

/* 禁用控制台输出到屏幕 (仍写入日志) */
void console_disable_output(void);

/* 启用控制台输出 */
void console_enable_output(void);

/* 串口输出 (用于调试) */
void serial_print(const char *str);
void serial_putchar(char c);
void serial_print_hex(uint64_t val);
int serial_getchar(void);  /* 从串口读取字符，无输入返回 -1 */

/* 设置颜色 */
void console_set_color(uint32_t fg, uint32_t bg);

/* ═══════════════════════════════════════════════════════════════════════════
 * ZRATRO UNIQUE LOG SYSTEM - 独特日志风格
 * ═══════════════════════════════════════════════════════════════════════════ */

/*─── 日志级别 ───*/
typedef enum {
    Z_LOG_LEVEL_TRACE   = 0,
    Z_LOG_LEVEL_DEBUG   = 1,
    Z_LOG_LEVEL_INFO    = 2,
    Z_LOG_LEVEL_WARN    = 3,
    Z_LOG_LEVEL_ERROR   = 4,
    Z_LOG_LEVEL_FATAL   = 5
} z_log_level_t;

/*─── Zratro 独特格式化日志 ───*/
void z_log(z_log_level_t level, const char *fmt, ...);
void z_log_raw(const char *fmt, ...);

/*─── Zratro 独特状态输出 ───*/
#define Z_LOG_SUCCESS(msg)      console_success(msg)
#define Z_LOG_INFO_MSG(msg)     console_info(msg)
#define Z_LOG_WARNING(msg)      console_warning(msg)
#define Z_LOG_ERROR_MSG(msg)    console_error(msg)
#define Z_LOG_DEBUG_MSG(msg)    console_debug(msg)

/*─── Zratro 独特区块输出 ───*/
void z_print_block_start(const char *title);
void z_print_block_end(const char *title);
void z_print_separator_line(char pattern, int length);
void z_print_key_value(const char *key, const char *value, uint32_t key_color);
void z_print_key_value_hex(const char *key, uint64_t value, uint32_t key_color);
void z_print_key_value_dec(const char *key, int64_t value, uint32_t key_color);
void z_print_timestamped(const char *msg);

/*─── Zratro 独特进度输出 ───*/
void z_progress_start(const char *operation);
void z_progress_update(int percent, const char *status);
void z_progress_end(int success, const char *result);

/*─── Zratro 独特调试输出 ───*/
void z_dump_hex(const void *data, size_t len, const char *label);
void z_dump_stack(const void *stack, size_t depth);
void z_dump_registers(const void *regs);

/*─── Zratro 独特表格输出 ───*/
void z_table_start(const char *headers[], int col_count, int col_widths[]);
void z_table_row(const char *values[], int col_count);
void z_table_end(void);

/* ========== 现代化调试输出函数 ========== */

/* 打印带状态图标的日志 (level: 0=成功, 1=信息, 2=警告, 3=错误, 4=调试) */
void console_log_status(int level, const char *tag, const char *msg);

/* 打印阶段标题 */
void console_print_header(const char *title);

/* 打印分隔线 */
void console_print_separator(void);

/* 打印键值对 */
void console_print_kv(const char *key, const char *value);

/* 打印十六进制键值对 */
void console_print_kv_hex(const char *key, uint64_t value);

/* 打印十进制键值对 */
void console_print_kv_dec(const char *key, int64_t value);

/* 打印进度条 */
void console_print_progress(int percent, const char *label);

/* 快捷日志函数 */
void console_success(const char *msg);
void console_info(const char *msg);
void console_warning(const char *msg);
void console_error(const char *msg);
void console_debug(const char *msg);

/* ========== 简洁彩色调试输出 (实机无串口时直接显示在屏幕上) ==========
 * 自动加换行, 同时输出到屏幕、串口、日志缓冲区:
 *   screen_error -> 红色 (致命错误)
 *   screen_warn  -> 黄色 (警告)
 *   screen_info  -> 白色 (普通信息)
 *   screen_ok    -> 绿色 (成功) */
void screen_error(const char *msg);
void screen_warn(const char *msg);
void screen_info(const char *msg);
void screen_ok(const char *msg);

/* ========== 内核日志功能 ========== */

/* 初始化日志系统 */
void klog_init(void);

/* 刷新日志缓冲区到文件 (文件系统就绪后调用) */
void klog_flush_to_file(void);

/* 关闭日志系统 */
void klog_close(void);

/* 获取日志缓冲区大小 */
size_t klog_get_size(void);

/* 检查日志系统是否启用 */
int klog_is_enabled(void);

/* 获取日志缓冲区指针和大小 (用于调试导出) */
const char *klog_get_buffer(size_t *size_out);

/* 打印日志统计信息 */
void klog_print_stats(void);

/* 日志级别控制 */
void klog_set_screen_level(z_log_level_t level);
void klog_set_serial_level(z_log_level_t level);
z_log_level_t klog_get_screen_level(void);

/* ═══════════════════════════════════════════════════════════════════════════
 * KERNEL PANIC - 致命错误立即终止
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 致命错误 - 立即停止内核 (永不返回) */
__attribute__((noreturn))
void kernel_panic(const char *msg);

/* 便捷宏: 检查条件，失败则 panic */
#define PANIC_IF(cond, msg) do { if (cond) kernel_panic(msg); } while(0)

/* 便捷宏: 检查指针非空 */
#define PANIC_IF_NULL(ptr, msg) PANIC_IF((ptr) == NULL, msg)

/* ═══════════════════════════════════════════════════════════════════════════
 * 中文调试宏 - Chinese Debug Macros
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 中文日志级别前缀 */
#define ZH_LOG_TRACE   "[跟踪]"
#define ZH_LOG_DEBUG   "[调试]"
#define ZH_LOG_INFO    "[信息]"
#define ZH_LOG_WARN    "[警告]"
#define ZH_LOG_ERROR   "[错误]"
#define ZH_LOG_FATAL   "[致命]"

/* 中文状态标识 */
#define ZH_SUCCESS   "成功"
#define ZH_FAILED    "失败"
#define ZH_OK        "正常"
#define ZH_ERR       "错误"
#define ZH_WARNING   "警告"
#define ZH_INFO      "信息"
#define ZH_START     "开始"
#define ZH_END       "结束"
#define ZH_INIT      "初始化"
#define ZH_LOAD      "加载"
#define ZH_CONFIG    "配置"
#define ZH_ENABLE    "启用"
#define ZH_DISABLE   "禁用"
#define ZH_READY     "就绪"
#define ZH_RUNNING   "运行中"
#define ZH_STOPPED   "已停止"

/* 中文硬件术语 */
#define ZH_MEMORY    "内存"
#define ZH_PROCESS   "进程"
#define ZH_THREAD    "线程"
#define ZH_DEVICE    "设备"
#define ZH_DRIVER    "驱动"
#define ZH_KERNEL    "内核"
#define ZH_SYSTEM   "系统"
#define ZH_CPU       "处理器"
#define ZH_DISK      "磁盘"
#define ZH_NETWORK  "网络"

/* 中文调试宏 - 简化版 (适用于当前 z_log 实现) */
#define ZH_TRACE_LOG(fmt)   println(ZH_LOG_TRACE " " fmt)
#define ZH_DEBUG_LOG(fmt)   println(ZH_LOG_DEBUG " " fmt)
#define ZH_INFO_LOG(fmt)    println(ZH_LOG_INFO " " fmt)
#define ZH_WARN_LOG(fmt)    println(ZH_LOG_WARN " " fmt)
#define ZH_ERROR_LOG(fmt)   println(ZH_LOG_ERROR " " fmt)
#define ZH_FATAL_LOG(fmt)   println(ZH_LOG_FATAL " " fmt)

/* 中文带标签的调试宏 */
#define ZH_TRACE_TAG(tag, fmt)   println(ZH_LOG_TRACE " [" tag "] " fmt)
#define ZH_DEBUG_TAG(tag, fmt)   println(ZH_LOG_DEBUG " [" tag "] " fmt)
#define ZH_INFO_TAG(tag, fmt)    println(ZH_LOG_INFO " [" tag "] " fmt)
#define ZH_WARN_TAG(tag, fmt)    println(ZH_LOG_WARN " [" tag "] " fmt)
#define ZH_ERROR_TAG(tag, fmt)   println(ZH_LOG_ERROR " [" tag "] " fmt)

/* 中文状态输出宏 */
#define ZH_LOG_START(module)     println("[" module "] " ZH_START "...")
#define ZH_LOG_END(module)       println("[" module "] " ZH_END)
#define ZH_LOG_SUCCESS_MSG(module, msg)  println("[" module "] " ZH_SUCCESS ": " msg)
#define ZH_LOG_FAILED_MSG(module, msg)   println("[" module "] " ZH_FAILED ": " msg)
#define ZH_LOG_INFO_MSG_ZH(module, msg)  println("[" module "] " ZH_INFO ": " msg)

/* 中文键值对输出 */
#define ZH_KV(key, value)       do { print("  " key ": "); println(value); } while(0)
#define ZH_KV_HEX(key, value)   do { print("  " key ": 0x"); print_hex(value); println(""); } while(0)
#define ZH_KV_DEC(key, value)   do { print("  " key ": "); print_dec(value); println(""); } while(0)

#endif