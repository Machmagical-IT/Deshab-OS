#ifndef STDIO_H
#define STDIO_H

#include "types.h"

/* 标准文件描述符 */
#define STDIN_FILENO    0
#define STDOUT_FILENO   1
#define STDERR_FILENO   2

/* 文件结构 */
#define FILE_BU_SIZE    4096

typedef struct {
    int fd;
    int flags;
    uint8_t *buffer;
    size_t buf_size;
    size_t buf_pos;
    int eof;
    int error;
} FILE;

/* 文件标志 */
#define _IOFBF  0   /* 全缓冲 */
#define _IOLBF  1   /* 行缓冲 */
#define _IONBF  2   /* 无缓冲 */

/* 标准流 */
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

/* 打印函数 */
int puts(const char *s);
int printf(const char *fmt, ...);
int sprintf(char *str, const char *fmt, ...);
int snprintf(char *str, size_t size, const char *fmt, ...);
int putchar(int c);
int fputs(const char *s, int fd);
int fputc(int c, int fd);
int fputs_stdout(const char *s);
int fputs_stderr(const char *s);

/* 打印整数 */
void print_int(int64_t value);
void print_uint(uint64_t value);
void print_hex(uint64_t value);
void print_oct(uint64_t value);

/* 读取函数 */
char *gets(char *s, int size);
int getchar(void);

/* 文件操作 */
FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int fclose(FILE *fp);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *fp);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp);
int fseek(FILE *fp, long offset, int whence);
long ftell(FILE *fp);
void rewind(FILE *fp);
int feof(FILE *fp);
int ferror(FILE *fp);
void clearerr(FILE *fp);
int fflush(FILE *fp);
int fileno(FILE *fp);

/* 错误处理 */
void perror(const char *s);
const char *strerror(int errnum);

/* 缓冲控制 */
int setvbuf(FILE *fp, char *buf, int mode, size_t size);
void setbuf(FILE *fp, char *buf);

#endif