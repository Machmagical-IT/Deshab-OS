#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>
#include <stddef.h>

/* 图片格式 */
typedef enum {
    IMAGE_FORMAT_UNKNOWN = 0,
    IMAGE_FORMAT_BMP,
    IMAGE_FORMAT_PNG,
    IMAGE_FORMAT_JPEG,
    IMAGE_FORMAT_GIF,
    IMAGE_FORMAT_TGA,
} image_format_t;

/* 图片信息 */
struct image_info {
    int width;
    int height;
    int bpp;            /* 每像素位数 */
    int channels;       /* 通道数 */
    int has_alpha;      /* 是否有透明通道 */
    image_format_t format;
};

/* 图片数据 */
struct image {
    struct image_info info;
    uint32_t *pixels;   /* ARGB32 格式 */
    size_t size;        /* 数据大小 */
};

/* 初始化图片系统 */
int image_init(void);

/* 加载图片 */
struct image *image_load(const char *path);
struct image *image_load_from_memory(const void *data, size_t size);

/* 释放图片 */
void image_free(struct image *img);

/* 获取图片信息 */
int image_get_info(struct image *img, struct image_info *info);

/* 获取像素 */
uint32_t image_get_pixel(struct image *img, int x, int y);
void image_set_pixel(struct image *img, int x, int y, uint32_t pixel);

/* 图片操作 */
struct image *image_resize(struct image *img, int new_width, int new_height);
struct image *image_crop(struct image *img, int x, int y, int w, int h);
struct image *image_rotate(struct image *img, int degrees);
struct image *image_flip_horizontal(struct image *img);
struct image *image_flip_vertical(struct image *img);

/* 图片滤镜 */
void image_grayscale(struct image *img);
void image_invert(struct image *img);
void image_brightness(struct image *img, int brightness);
void image_contrast(struct image *img, int contrast);
void image_blur(struct image *img, int radius);
void image_sharpen(struct image *img);

/* BMP 解码器 */
struct image *bmp_decode(const void *data, size_t size);

/* PNG 解码器 (简化版) */
struct image *png_decode(const void *data, size_t size);

/* TGA 解码器 */
struct image *tga_decode(const void *data, size_t size);

/* JPEG 解码器 */
struct image *jpeg_decode(const void *data, size_t size);

/* 检测图片格式 */
image_format_t image_detect_format(const void *data, size_t size);

#endif
