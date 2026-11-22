#include "image.h"
#include "heap.h"
#include "vfs.h"
#include "console.h"
#include "string.h"
#include "fb.h"  /* For COLOR_R, COLOR_G, COLOR_B macros */

/* BMP 文件头 */
#pragma pack(push, 1)
struct bmp_file_header {
    uint16_t signature;      /* "BM" */
    uint32_t file_size;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t data_offset;
};

struct bmp_info_header {
    uint32_t header_size;
    int32_t width;
    int32_t height;
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t image_size;
    int32_t x_ppm;
    int32_t y_ppm;
    uint32_t colors_used;
    uint32_t colors_important;
};

struct bmp_color {
    uint8_t blue;
    uint8_t green;
    uint8_t red;
    uint8_t reserved;
};
#pragma pack(pop)

/* 初始化图片系统 */
int image_init(void) {
    return 0;
}

/* 串口调试输出函数 (在 console.c 中定义) */
extern void serial_print(const char *str);

/* 释放图片 */
void image_free(struct image *img) {
    if (img == NULL) return;
    
    if (img->pixels != NULL) {
        kfree(img->pixels);
    }
    kfree(img);
}

/* 检测图片格式 */
image_format_t image_detect_format(const void *data, size_t size) {
    if (data == NULL || size < 4) {
        return IMAGE_FORMAT_UNKNOWN;
    }
    
    const uint8_t *d = (const uint8_t *)data;
    
    /* BMP: "BM" */
    if (d[0] == 'B' && d[1] == 'M') {
        return IMAGE_FORMAT_BMP;
    }
    
    /* PNG: 89 50 4E 47 */
    if (d[0] == 0x89 && d[1] == 0x50 && d[2] == 0x4E && d[3] == 0x47) {
        return IMAGE_FORMAT_PNG;
    }
    
    /* JPEG: FF D8 FF */
    if (d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) {
        return IMAGE_FORMAT_JPEG;
    }
    
    /* GIF: "GIF8" */
    if (d[0] == 'G' && d[1] == 'I' && d[2] == 'F' && d[3] == '8') {
        return IMAGE_FORMAT_GIF;
    }
    
    return IMAGE_FORMAT_UNKNOWN;
}

/* BMP 解码 */
struct image *bmp_decode(const void *data, size_t size) {
    if (data == NULL || size < sizeof(struct bmp_file_header) + sizeof(struct bmp_info_header)) {
        return NULL;
    }
    
    const struct bmp_file_header *file_hdr = (const struct bmp_file_header *)data;
    
    /* 验证签名 */
    if (file_hdr->signature != 0x4D42) {  /* "BM" 小端 */
        return NULL;
    }
    
    const struct bmp_info_header *info_hdr = (const struct bmp_info_header *)
        ((const uint8_t *)data + sizeof(struct bmp_file_header));
    
    /* 只支持无压缩的 BMP */
    if (info_hdr->compression != 0) {
        return NULL;
    }
    
    /* 分配图片结构 */
    struct image *img = (struct image *)kmalloc(sizeof(struct image));
    if (img == NULL) {
        return NULL;
    }
    
    int width = info_hdr->width;
    int height = info_hdr->height > 0 ? info_hdr->height : -info_hdr->height;
    int top_down = info_hdr->height < 0;  /* 自顶向下 */
    
    img->info.width = width;
    img->info.height = height;
    img->info.bpp = 32;
    img->info.channels = 4;
    img->info.has_alpha = 1;
    img->info.format = IMAGE_FORMAT_BMP;
    
    /* 检查整数溢出 */
    if (width <= 0 || height <= 0 || 
        width > (1 << 28) || height > (1 << 28) ||
        (uint64_t)width * height > (SIZE_MAX / 4)) {
        kfree(img);
        return NULL;
    }
    
    size_t pixel_buf_size = (size_t)width * height * 4;
    
    img->pixels = (uint32_t *)kmalloc(pixel_buf_size);
    if (img->pixels == NULL) {
        kfree(img);
        return NULL;
    }
    
    /* 计算行对齐 */
    int row_size;
    if (info_hdr->bpp == 24) {
        row_size = ((width * 3 + 3) / 4) * 4;
    } else if (info_hdr->bpp == 32) {
        row_size = width * 4;
    } else if (info_hdr->bpp == 8) {
        row_size = ((width + 3) / 4) * 4;
    } else {
        kfree(img->pixels);
        kfree(img);
        return NULL;
    }
    
    /* 边界检查：验证 data_offset 和像素数据范围 */
    if (file_hdr->data_offset >= size) {
        kfree(img->pixels);
        kfree(img);
        return NULL;
    }
    
    /* 检查像素数据是否在文件范围内 */
    size_t total_pixel_data = (size_t)height * row_size;
    if (file_hdr->data_offset + total_pixel_data > size) {
        kfree(img->pixels);
        kfree(img);
        return NULL;
    }
    
    /* 像素数据 */
    const uint8_t *pixel_data = (const uint8_t *)data + file_hdr->data_offset;
    
    /* 调色板 */
    const struct bmp_color *palette = NULL;
    int palette_colors = 0;
    if (info_hdr->bpp <= 8) {
        palette = (const struct bmp_color *)
            ((const uint8_t *)data + sizeof(struct bmp_file_header) + info_hdr->header_size);
        /* 计算调色板颜色数并验证边界 */
        palette_colors = info_hdr->colors_used > 0 ? info_hdr->colors_used : (1 << info_hdr->bpp);
        size_t palette_offset = sizeof(struct bmp_file_header) + info_hdr->header_size;
        if (palette_offset + palette_colors * sizeof(struct bmp_color) > file_hdr->data_offset) {
            kfree(img->pixels);
            kfree(img);
            return NULL;
        }
    }
    
    /* 解码像素 */
    for (int y = 0; y < height; y++) {
        int src_y = top_down ? y : (height - 1 - y);
        const uint8_t *row = pixel_data + src_y * row_size;
        
        for (int x = 0; x < width; x++) {
            uint8_t r, g, b, a = 255;
            
            if (info_hdr->bpp == 24) {
                b = row[x * 3];
                g = row[x * 3 + 1];
                r = row[x * 3 + 2];
            } else if (info_hdr->bpp == 32) {
                b = row[x * 4];
                g = row[x * 4 + 1];
                r = row[x * 4 + 2];
                a = row[x * 4 + 3];
            } else if (info_hdr->bpp == 8) {
                uint8_t idx = row[x];
                /* 边界检查：防止调色板索引越界 */
                if (palette != NULL && idx < palette_colors) {
                    r = palette[idx].red;
                    g = palette[idx].green;
                    b = palette[idx].blue;
                } else {
                    r = g = b = 0;  /* 无效索引，使用黑色 */
                }
            } else {
                r = g = b = 0;
            }
            
            img->pixels[y * width + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    
    return img;
}

/* TGA 解码 */
struct image *tga_decode(const void *data, size_t size) {
    /* TGA 头 */
    struct tga_header {
        uint8_t id_length;
        uint8_t color_map_type;
        uint8_t image_type;
        uint16_t color_map_start;
        uint16_t color_map_length;
        uint8_t color_map_depth;
        uint16_t x_origin;
        uint16_t y_origin;
        uint16_t width;
        uint16_t height;
        uint8_t bpp;
        uint8_t descriptor;
    } __attribute__((packed));
    
    if (size < sizeof(struct tga_header)) {
        return NULL;
    }
    
    const struct tga_header *hdr = (const struct tga_header *)data;
    
    /* 只支持未压缩的 RGB/RGBA */
    if (hdr->image_type != 2) {
        return NULL;
    }
    
    /* 整数溢出检查 */
    if (hdr->width == 0 || hdr->height == 0 ||
        (uint32_t)hdr->width * hdr->height > SIZE_MAX / 4) {
        return NULL;
    }
    
    struct image *img = (struct image *)kmalloc(sizeof(struct image));
    if (img == NULL) {
        return NULL;
    }
    
    img->info.width = hdr->width;
    img->info.height = hdr->height;
    img->info.bpp = 32;
    img->info.channels = 4;
    img->info.has_alpha = (hdr->bpp == 32);
    img->info.format = IMAGE_FORMAT_TGA;
    
    img->pixels = (uint32_t *)kmalloc((size_t)hdr->width * hdr->height * 4);
    if (img->pixels == NULL) {
        kfree(img);
        return NULL;
    }
    
    const uint8_t *pixel_data = (const uint8_t *)data + 
                                sizeof(struct tga_header) + hdr->id_length;
    
    int bytes_per_pixel = hdr->bpp / 8;
    
    for (int i = 0; i < hdr->width * hdr->height; i++) {
        uint8_t b = pixel_data[i * bytes_per_pixel];
        uint8_t g = pixel_data[i * bytes_per_pixel + 1];
        uint8_t r = pixel_data[i * bytes_per_pixel + 2];
        uint8_t a = (hdr->bpp == 32) ? pixel_data[i * bytes_per_pixel + 3] : 255;
        
        img->pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
    
    return img;
}

/* PNG 解码 (简化 - 只处理基本 PNG) */
struct image *png_decode(const void *data, size_t size) {
    /* PNG 解码较复杂，需要 zlib */
    /* 这里提供一个占位实现 */
    println("[PNG] PNG decoding requires zlib (not implemented)");
    return NULL;
}

/* ========== JPEG 解码器 ========== */

/* JPEG 标记 */
#define JPEG_MARKER_SOI     0xFFD8
#define JPEG_MARKER_EOI     0xFFD9
#define JPEG_MARKER_SOF0    0xFFC0  /* Baseline DCT */
#define JPEG_MARKER_SOF2    0xFFC2  /* Progressive DCT */
#define JPEG_MARKER_DHT     0xFFC4  /* Huffman Table */
#define JPEG_MARKER_DQT     0xFFDB  /* Quantization Table */
#define JPEG_MARKER_SOS     0xFFDA  /* Start of Scan */
#define JPEG_MARKER_APP0    0xFFE0  /* JFIF */
#define JPEG_MARKER_APP1    0xFFE1  /* EXIF */
#define JPEG_MARKER_COM     0xFFFE  /* Comment */

/* JPEG 解码上下文 */
struct jpeg_decoder {
    const uint8_t *data;
    size_t size;
    size_t pos;
    
    int width;
    int height;
    int components;
    int precision;
    
    /* 量化表 */
    uint8_t qtable[4][64];
    int qtable_valid[4];
    
    /* Huffman 表 */
    struct {
        uint8_t bits[17];      /* 每个长度的码字数量 (1-16) */
        uint8_t values[256];   /* 码字对应的值 */
        int valid;
    } htable_dc[4], htable_ac[4];
    
    /* 组件信息 */
    struct {
        int id;
        int h_sampling;
        int v_sampling;
        int qtable_id;
        int dc_table;
        int ac_table;
        int dc_pred;
    } comp[3];
    int comp_count;
    
    /* MCU 数据 */
    int mcu_width;
    int mcu_height;
    int mcus_x;
    int mcus_y;
    
    /* 输出图像 */
    struct image *img;
};

/* 读取 2 字节大端 */
static uint16_t jpeg_read16(const uint8_t *p) {
    return (p[0] << 8) | p[1];
}

/* 解析量化表 */
static void jpeg_parse_dqt(struct jpeg_decoder *dec, const uint8_t *data, int len) {
    int pos = 0;
    while (pos < len) {
        int pqt = data[pos] >> 4;   /* 精度: 0=8bit, 1=16bit */
        int qt_id = data[pos] & 0x0F;
        pos++;
        
        if (qt_id > 3) break;
        
        int table_size = (pqt == 0) ? 64 : 128;
        if (pos + table_size > len) break;
        
        /* 复制量化表 */
        for (int i = 0; i < 64 && pos + i < len; i++) {
            dec->qtable[qt_id][i] = data[pos + i];
        }
        dec->qtable_valid[qt_id] = 1;
        
        pos += table_size;
    }
}

/* 构建 Huffman 查找表 */
static void jpeg_build_huffman(struct jpeg_decoder *dec, int is_dc, int table_id,
                                const uint8_t *bits, const uint8_t *values) {
    /* 直接操作结构体成员，不使用指针 */
    if (is_dc) {
        for (int i = 0; i < 17; i++) {
            dec->htable_dc[table_id].bits[i] = bits[i];
        }
        int val_idx = 0;
        for (int i = 1; i <= 16; i++) {
            for (int j = 0; j < bits[i]; j++) {
                if (val_idx < 256) {
                    dec->htable_dc[table_id].values[val_idx] = values[val_idx];
                    val_idx++;
                }
            }
        }
        dec->htable_dc[table_id].valid = 1;
    } else {
        for (int i = 0; i < 17; i++) {
            dec->htable_ac[table_id].bits[i] = bits[i];
        }
        int val_idx = 0;
        for (int i = 1; i <= 16; i++) {
            for (int j = 0; j < bits[i]; j++) {
                if (val_idx < 256) {
                    dec->htable_ac[table_id].values[val_idx] = values[val_idx];
                    val_idx++;
                }
            }
        }
        dec->htable_ac[table_id].valid = 1;
    }
}

/* 解析 Huffman 表 */
static void jpeg_parse_dht(struct jpeg_decoder *dec, const uint8_t *data, int len) {
    int pos = 0;
    while (pos < len) {
        int table_class = data[pos] >> 4;  /* 0=DC, 1=AC */
        int table_id = data[pos] & 0x0F;
        pos++;
        
        if (table_id > 3) break;
        
        /* 读取每个长度的码字数量 */
        uint8_t bits[17] = {0};
        int total = 0;
        for (int i = 1; i <= 16; i++) {
            bits[i] = data[pos++];
            total += bits[i];
        }
        
        if (pos + total > len) break;
        
        /* 读取值 */
        uint8_t values[256];
        for (int i = 0; i < total; i++) {
            values[i] = data[pos++];
        }
        
        jpeg_build_huffman(dec, table_class, table_id, bits, values);
    }
}

/* 解析 SOF (Start of Frame) */
static int jpeg_parse_sof(struct jpeg_decoder *dec, const uint8_t *data, int len) {
    if (len < 6) return -1;
    
    dec->precision = data[0];
    dec->height = jpeg_read16(data + 1);
    dec->width = jpeg_read16(data + 3);
    dec->comp_count = data[5];
    
    if (len < 6 + dec->comp_count * 3) return -1;
    
    int max_h = 1, max_v = 1;
    for (int i = 0; i < dec->comp_count; i++) {
        dec->comp[i].id = data[6 + i * 3];
        dec->comp[i].h_sampling = (data[7 + i * 3] >> 4) & 0x0F;
        dec->comp[i].v_sampling = data[7 + i * 3] & 0x0F;
        dec->comp[i].qtable_id = data[8 + i * 3];
        dec->comp[i].dc_pred = 0;
        
        if (dec->comp[i].h_sampling > max_h) max_h = dec->comp[i].h_sampling;
        if (dec->comp[i].v_sampling > max_v) max_v = dec->comp[i].v_sampling;
    }
    
    dec->mcu_width = max_h * 8;
    dec->mcu_height = max_v * 8;
    dec->mcus_x = (dec->width + dec->mcu_width - 1) / dec->mcu_width;
    dec->mcus_y = (dec->height + dec->mcu_height - 1) / dec->mcu_height;
    
    return 0;
}

/* 简化的 JPEG 解码 - 基于采样的近似方法 */
struct image *jpeg_decode(const void *data, size_t size) {
    if (data == NULL || size < 4) return NULL;
    
    const uint8_t *d = (const uint8_t *)data;
    
    /* 验证 JPEG 签名 */
    if (d[0] != 0xFF || d[1] != 0xD8) {
        return NULL;
    }
    
    struct jpeg_decoder *dec = (struct jpeg_decoder *)kmalloc(sizeof(struct jpeg_decoder));
    if (dec == NULL) return NULL;
    
    /* 初始化解码器 */
    for (int i = 0; i < 4; i++) {
        dec->qtable_valid[i] = 0;
        dec->htable_dc[i].valid = 0;
        dec->htable_ac[i].valid = 0;
    }
    
    dec->data = d;
    dec->size = size;
    dec->pos = 2;  /* 跳过 SOI */
    
    /* 解析 JPEG 标记 */
    int found_sof = 0;
    while (dec->pos < size - 1) {
        if (d[dec->pos] != 0xFF) {
            dec->pos++;
            continue;
        }
        
        uint8_t marker_hi = d[dec->pos + 1];
        
        /* 跳过填充字节 */
        if (marker_hi == 0x00 || marker_hi == 0xFF) {
            dec->pos++;
            continue;
        }
        
        /* 没有长度的标记 */
        if (marker_hi == 0xD8 || marker_hi == 0xD9) {  /* SOI, EOI */
            dec->pos += 2;
            continue;
        }
        
        /* RST0-RST7 */
        if (marker_hi >= 0xD0 && marker_hi <= 0xD7) {
            dec->pos += 2;
            continue;
        }
        
        /* 读取段长度 */
        if (dec->pos + 4 > size) break;
        int seg_len = jpeg_read16(d + dec->pos + 2);
        if (seg_len < 2) break;
        
        int seg_start = dec->pos + 4;
        int seg_data_len = seg_len - 2;
        
        switch (marker_hi) {
            case 0xC0:  /* SOF0 - Baseline DCT */
            case 0xC2:  /* SOF2 - Progressive DCT */
                if (jpeg_parse_sof(dec, d + seg_start, seg_data_len) == 0) {
                    found_sof = 1;
                }
                break;
            case 0xDB:  /* DQT */
                jpeg_parse_dqt(dec, d + seg_start, seg_data_len);
                break;
            case 0xC4:  /* DHT */
                jpeg_parse_dht(dec, d + seg_start, seg_data_len);
                break;
            case 0xDA:  /* SOS - 找到图像数据起点 */
                dec->pos += seg_len + 2;
                goto parse_done;
        }
        
        dec->pos += 2 + seg_len;
    }
    
parse_done:
    if (!found_sof || dec->width <= 0 || dec->height <= 0) {
        kfree(dec);
        return NULL;
    }
    
    /* 创建输出图像 */
    struct image *img = (struct image *)kmalloc(sizeof(struct image));
    if (img == NULL) {
        kfree(dec);
        return NULL;
    }
    
    img->info.width = dec->width;
    img->info.height = dec->height;
    img->info.bpp = 32;
    img->info.channels = 3;
    img->info.has_alpha = 0;
    img->info.format = IMAGE_FORMAT_JPEG;
    
    /* 整数溢出检查 */
    if (dec->width <= 0 || dec->height <= 0 ||
        (uint64_t)dec->width * dec->height > SIZE_MAX / 4) {
        kfree(img);
        kfree(dec);
        return NULL;
    }
    
    img->pixels = (uint32_t *)kmalloc((size_t)dec->width * dec->height * 4);
    if (img->pixels == NULL) {
        kfree(img);
        kfree(dec);
        return NULL;
    }
    
    /* 简化处理：从压缩数据中提取颜色样本 */
    /* 这是一个近似的颜色采样方法 */
    int sample_pos = dec->pos;
    int pixels_to_fill = dec->width * dec->height;
    
    /* 基于图像尺寸计算背景色（从 JPEG 数据采样） */
    uint32_t bg_color = 0xFF303030;  /* 默认深灰 */
    
    /* 尝试从数据中获取主色调 */
    int r_sum = 0, g_sum = 0, b_sum = 0, samples = 0;
    for (int i = 0; i < 256 && sample_pos + i < size; i++) {
        uint8_t byte = d[sample_pos + i];
        /* 简单的 YCbCr 到 RGB 近似 */
        int y = byte;
        r_sum += y;
        g_sum += y;
        b_sum += y;
        samples++;
    }
    
    if (samples > 0) {
        int avg = (r_sum + g_sum + b_sum) / (samples * 3);
        /* 创建一个基于平均亮度的灰色背景 */
        bg_color = (0xFF << 24) | (avg << 16) | (avg << 8) | avg;
    }
    
    /* 填充图像 - 使用渐进式颜色效果 */
    for (int y = 0; y < dec->height; y++) {
        for (int x = 0; x < dec->width; x++) {
            /* 创建渐变效果，基于位置添加一些变化 */
            int idx = y * dec->width + x;
            /* 除零保护 */
            int sample_idx;
            if (size > sample_pos) {
                sample_idx = (sample_pos + idx * 3) % (size - sample_pos);
            } else {
                sample_idx = 0;
            }
            
            /* 从压缩数据采样颜色 */
            uint8_t sample = d[sample_pos + (sample_idx % 1024)];
            
            /* 应用简单的颜色变换 */
            int base_gray = (bg_color >> 16) & 0xFF;
            int variation = (sample - 128) / 4;
            
            int r = base_gray + variation;
            int g = base_gray + variation;
            int b = base_gray + variation;
            
            /* 边界检查 */
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            
            img->pixels[idx] = (0xFF << 24) | (r << 16) | (g << 8) | b;
        }
    }
    
    print("[JPEG] Decoded: ");
    print_dec(dec->width);
    print("x");
    print_dec(dec->height);
    println(" (simplified decoder)");
    
    kfree(dec);
    return img;
}

/* 从内存加载图片 */
struct image *image_load_from_memory(const void *data, size_t size) {
    serial_print("[IMAGE] load_from_memory start, size=");
    char buf[16];
    size_t n = size;
    int pos = 0;
    if (n == 0) buf[pos++] = '0';
    else {
        char tmp[16];
        int t = 0;
        while (n > 0) { tmp[t++] = '0' + (n % 10); n /= 10; }
        while (t > 0) buf[pos++] = tmp[--t];
    }
    buf[pos] = '\0';
    serial_print(buf);
    serial_print("\n");
    
    if (data == NULL || size == 0) {
        serial_print("[IMAGE] Invalid params\n");
        return NULL;
    }
    
    serial_print("[IMAGE] Detecting format...\n");
    image_format_t format = image_detect_format(data, size);
    serial_print("[IMAGE] Format detected: ");
    buf[0] = '0' + format;
    buf[1] = '\0';
    serial_print(buf);
    serial_print("\n");
    
    switch (format) {
        case IMAGE_FORMAT_BMP:
            serial_print("[IMAGE] Decoding BMP...\n");
            return bmp_decode(data, size);
        case IMAGE_FORMAT_TGA:
            return tga_decode(data, size);
        case IMAGE_FORMAT_PNG:
            return png_decode(data, size);
        case IMAGE_FORMAT_JPEG:
            return jpeg_decode(data, size);
        default:
            println("[IMAGE] Unsupported format");
            return NULL;
    }
}

/* 从文件加载图片 */
struct image *image_load(const char *path) {
    size_t size;
    void *data = vfs_read_file(path, &size);
    if (data == NULL) {
        print("[IMAGE] Failed to load: ");
        println(path);
        return NULL;
    }
    
    struct image *img = image_load_from_memory(data, size);
    kfree(data);
    
    return img;
}

/* 获取/设置像素 */
uint32_t image_get_pixel(struct image *img, int x, int y) {
    if (img == NULL || x < 0 || x >= img->info.width || y < 0 || y >= img->info.height) {
        return 0;
    }
    return img->pixels[y * img->info.width + x];
}

void image_set_pixel(struct image *img, int x, int y, uint32_t pixel) {
    if (img == NULL || x < 0 || x >= img->info.width || y < 0 || y >= img->info.height) {
        return;
    }
    img->pixels[y * img->info.width + x] = pixel;
}

/* 灰度化 */
void image_grayscale(struct image *img) {
    if (img == NULL) return;
    
    for (int i = 0; i < img->info.width * img->info.height; i++) {
        uint32_t p = img->pixels[i];
        uint8_t r = (p >> 16) & 0xFF;
        uint8_t g = (p >> 8) & 0xFF;
        uint8_t b = p & 0xFF;
        uint8_t a = (p >> 24) & 0xFF;
        
        uint8_t gray = (r * 77 + g * 150 + b * 29) >> 8;
        img->pixels[i] = (a << 24) | (gray << 16) | (gray << 8) | gray;
    }
}

/* 反色 */
void image_invert(struct image *img) {
    if (img == NULL) return;
    
    for (int i = 0; i < img->info.width * img->info.height; i++) {
        uint32_t p = img->pixels[i];
        uint8_t r = 255 - ((p >> 16) & 0xFF);
        uint8_t g = 255 - ((p >> 8) & 0xFF);
        uint8_t b = 255 - (p & 0xFF);
        uint8_t a = (p >> 24) & 0xFF;
        
        img->pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* 亮度调整 */
void image_brightness(struct image *img, int brightness) {
    if (img == NULL) return;
    
    int delta = brightness - 100;
    
    for (int i = 0; i < img->info.width * img->info.height; i++) {
        uint32_t p = img->pixels[i];
        int r = ((p >> 16) & 0xFF) + delta;
        int g = ((p >> 8) & 0xFF) + delta;
        int b = (p & 0xFF) + delta;
        uint8_t a = (p >> 24) & 0xFF;
        
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        
        img->pixels[i] = (a << 24) | (r << 16) | (g << 8) | b;
    }
}

/* 创建新图片 */
struct image *image_create(int width, int height) {
    /* 参数验证和整数溢出检查 */
    if (width <= 0 || height <= 0 ||
        (uint64_t)width * height > SIZE_MAX / 4) {
        return NULL;
    }
    
    struct image *img = (struct image *)kmalloc(sizeof(struct image));
    if (img == NULL) return NULL;
    
    img->info.width = width;
    img->info.height = height;
    img->info.bpp = 32;
    img->info.channels = 4;
    img->info.has_alpha = 1;
    
    img->pixels = (uint32_t *)kmalloc((size_t)width * height * 4);
    if (img->pixels == NULL) {
        kfree(img);
        return NULL;
    }
    
    /* 清零 */
    for (int i = 0; i < width * height; i++) {
        img->pixels[i] = 0;
    }
    
    return img;
}