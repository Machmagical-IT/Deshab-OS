#ifndef MP4_DECODE_H
#define MP4_DECODE_H

#include <stdint.h>
#include <stddef.h>

#define MP4_CODEC_UNKNOWN 0
#define MP4_CODEC_MJPEG   1
#define MP4_CODEC_H264    2

struct stsc_entry {
    uint32_t first_chunk;
    uint32_t samples_per_chunk;
    uint32_t sample_desc_idx;
};

struct mp4_info {
    int valid;
    int codec;
    int has_video;
    uint16_t width, height;
    uint32_t sample_count;
    uint32_t sample_size;
    uint32_t *sample_sizes;
    uint32_t chunk_count;
    uint64_t *chunk_offsets;
    uint32_t stsc_count;
    struct stsc_entry *stsc_entries;
    uint64_t mdat_offset;
    uint64_t mdat_size;
    void *file_data;
    size_t file_size;
};

int mp4_parse(const char *path, struct mp4_info *info);
int mp4_decode_frame(struct mp4_info *info, uint32_t frame_idx,
                     uint32_t *rgb_out, int out_w, int out_h);
void mp4_close(struct mp4_info *info);

int mjpeg_decode_frame(const uint8_t *data, size_t size,
                uint32_t *rgb_out, int out_w, int out_h);

#endif
