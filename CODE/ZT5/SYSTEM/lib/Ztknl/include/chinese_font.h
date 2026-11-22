#ifndef CHINESE_FONT_H
#define CHINESE_FONT_H

#include <stdint.h>

/* ═══════════════════════════════════════════════════════════════════════════
 * 中文字符支持 - 16x16 点阵字库
 * ═══════════════════════════════════════════════════════════════════════════ */

/* 中文字符尺寸 */
#define CN_CHAR_WIDTH  16
#define CN_CHAR_HEIGHT 16

/*
 * UTF-8 编码的中文字符点阵数据
 * 每个字符 32 字节 (16x16, 每行2字节)
 * 
 * 包含常用调试信息中文字符:
 * - 系统状态: 启动、初始化、成功、失败、错误、警告等
 * - 硬件相关: 内存、处理器、设备、驱动等
 * - 调试相关: 调试、信息、跟踪、级别等
 */

/* UTF-8 中文字符结构 */
struct cn_font_glyph {
    uint32_t utf8_code;  /* UTF-8 编码 (最多4字节) */
    uint8_t bitmap[32];  /* 16x16 点阵 (每行2字节) */
};

/* 常用中文字符点阵数据 */
extern const struct cn_font_glyph *cn_font_table;
extern const int cn_font_table_size;

/* UTF-8 解析函数 */
uint32_t utf8_decode(const char *str, int *bytes_read);

/* 查找中文字符点阵 */
const uint8_t* cn_font_find_char(uint32_t utf8_code);

/* 绘制中文字符 (由 console.c 实现) */
void cn_draw_char(uint32_t utf8_code, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);

/* 绘制中文文本 */
void cn_draw_text(const char *text, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);

/* 检查是否为 UTF-8 中文字符 (简单的多字节序列检测) */
static inline int is_utf8_chinese_start(uint8_t byte) {
    /* UTF-8 中文通常在 0xE4-0xE9 范围内开始 (CJK Unified Ideographs) */
    return (byte >= 0xE0 && byte <= 0xEF);
}

/* 获取 UTF-8 字符字节数 */
static inline int utf8_char_length(uint8_t first_byte) {
    if ((first_byte & 0x80) == 0x00) return 1;
    if ((first_byte & 0xE0) == 0xC0) return 2;
    if ((first_byte & 0xF0) == 0xE0) return 3;
    if ((first_byte & 0xF8) == 0xF0) return 4;
    return 1; /* 默认 */
}

#endif /* CHINESE_FONT_H */
