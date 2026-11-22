#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

/* 帧缓冲全局变量（从内核获取） */
extern uint32_t *fb_base;
extern uint32_t fb_width;
extern uint32_t fb_height;
extern uint32_t fb_pitch;

/* 基本控制台函数 */
void putchar(char c);
void print(const char *str);
void println(const char *str);

#endif
