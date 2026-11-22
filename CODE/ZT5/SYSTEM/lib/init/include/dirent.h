#ifndef DIRENT_H
#define DIRENT_H

#include "types.h"

/* 目录项结构 */
struct dirent {
    uint64_t d_ino;         /* inode 号 */
    uint64_t d_off;         /* 偏移量 */
    uint16_t d_reclen;      /* 记录长度 */
    uint8_t  d_type;        /* 文件类型 */
    char     d_name[256];   /* 文件名 */
};

/* 目录类型 */
#define DT_UNKNOWN   0
#define DT_FIFO      1
#define DT_CHR       2
#define DT_DIR       4
#define DT_BLK       6
#define DT_REG       8
#define DT_LNK      10
#define DT_SOCK     12
#define DT_WHT      14

/* 目录流 */
typedef struct {
    int fd;
    struct dirent entry;
    int eof;
} DIR;

/* 目录函数 */
DIR *opendir(const char *name);
struct dirent *readdir(DIR *dir);
int closedir(DIR *dir);
void rewinddir(DIR *dir);
long telldir(DIR *dir);
void seekdir(DIR *dir, long loc);

#endif
