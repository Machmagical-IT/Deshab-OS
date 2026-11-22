#include "mp4_decode.h"
#include "heap.h"
#include "string.h"
#include "vfs.h"
#include "console.h"
#include "vmm.h"

static uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

static uint16_t read_be16(const uint8_t *p) {
    return ((uint16_t)p[0]<<8)|p[1];
}

static int find_box(const uint8_t *data, size_t len, const char *type,
                    size_t *out_off, size_t *out_size) {
    size_t pos = 0;
    while (pos + 8 <= len) {
        uint32_t sz = read_be32(data + pos);
        if (sz == 0) sz = (uint32_t)(len - pos);
        if (sz < 8 || pos + sz > len) return 0;
        if (data[pos+4]==type[0] && data[pos+5]==type[1] &&
            data[pos+6]==type[2] && data[pos+7]==type[3]) {
            if (out_off) *out_off = pos;
            if (out_size) *out_size = sz;
            return 1;
        }
        pos += sz;
    }
    return 0;
}

static int parse_stbl(const uint8_t *data, size_t len,
                      struct mp4_info *info) {
    size_t off, sz;
    if (find_box(data, len, "stsd", &off, &sz)) {
        size_t stsd_payload = 8 + 8;
        if (off + stsd_payload + 8 <= len) {
            size_t entry_off = off + stsd_payload;
            uint32_t entry_sz = read_be32(data + entry_off);
            if (entry_sz >= 8 && entry_off + 8 <= len) {
                const uint8_t *codec = data + entry_off + 4;
                if (codec[0]=='m' && codec[1]=='j' && codec[2]=='p' && codec[3]=='g') {
                    info->codec = MP4_CODEC_MJPEG;
                    serial_print("[MP4] Codec: MJPEG\n");
                } else if (codec[0]=='a' && codec[1]=='v' && codec[2]=='c' && codec[3]=='1') {
                    info->codec = MP4_CODEC_H264;
                    serial_print("[MP4] Codec: H264 (not supported)\n");
                } else {
                    info->codec = MP4_CODEC_UNKNOWN;
                    serial_print("[MP4] Codec: unknown\n");
                }
            }
            if (entry_off + 48 <= len) {
                info->width = read_be16(data + entry_off + 32);
                info->height = read_be16(data + entry_off + 34);
                serial_print("[MP4] Video size: ");
                char wb[8]; int wp=0; uint16_t wv=info->width; if(wv==0){wb[wp++]='0';}else{char t[8];int tp=0;while(wv>0){t[tp++]='0'+(wv%10);wv/=10;}while(tp)wb[wp++]=t[--tp];} wb[wp]=0;
                serial_print(wb); serial_print("x");
                char hb[8]; int hp=0; uint16_t hv=info->height; if(hv==0){hb[hp++]='0';}else{char t[8];int tp=0;while(hv>0){t[tp++]='0'+(hv%10);hv/=10;}while(tp)hb[hp++]=t[--tp];} hb[hp]=0;
                serial_print(hb); serial_print("\n");
            }
        }
    }
    if (find_box(data, len, "stsz", &off, &sz)) {
        if (off + 12 <= len) {
            info->sample_size = read_be32(data + off + 8);
            info->sample_count = read_be32(data + off + 12);
            if (info->sample_size == 0 && info->sample_count > 0 &&
                off + 16 + info->sample_count * 4 <= len) {
                info->sample_sizes = (uint32_t *)kmalloc(info->sample_count * 4);
                if (info->sample_sizes) {
                    for (uint32_t i = 0; i < info->sample_count; i++) {
                        info->sample_sizes[i] = read_be32(data + off + 16 + i * 4);
                    }
                }
            }
            serial_print("[MP4] Samples: ");
            char sc[16]; int sp=0; uint32_t sv=info->sample_count; if(sv==0){sc[sp++]='0';}else{char t[16];int tp=0;while(sv>0){t[tp++]='0'+(sv%10);sv/=10;}while(tp)sc[sp++]=t[--tp];} sc[sp]='\n'; sc[sp+1]=0;
            serial_print(sc);
        }
    }
    if (find_box(data, len, "stco", &off, &sz)) {
        if (off + 12 <= len) {
            info->chunk_count = read_be32(data + off + 12);
            if (info->chunk_count > 0 && off + 16 + info->chunk_count * 4 <= len) {
                info->chunk_offsets = (uint64_t *)kmalloc(info->chunk_count * 8);
                if (info->chunk_offsets) {
                    for (uint32_t i = 0; i < info->chunk_count; i++) {
                        info->chunk_offsets[i] = read_be32(data + off + 16 + i * 4);
                    }
                }
            }
        }
    }
    if (find_box(data, len, "co64", &off, &sz)) {
        if (off + 12 <= len) {
            uint32_t cc = read_be32(data + off + 12);
            if (!info->chunk_offsets || cc > info->chunk_count) {
                if (info->chunk_offsets) kfree(info->chunk_offsets);
                info->chunk_count = cc;
                info->chunk_offsets = (uint64_t *)kmalloc(cc * 8);
                if (info->chunk_offsets && off + 16 + cc * 8 <= len) {
                    for (uint32_t i = 0; i < cc; i++) {
                        info->chunk_offsets[i] = read_be32(data + off + 16 + i * 8);
                        info->chunk_offsets[i] = (info->chunk_offsets[i] << 32) |
                                                  read_be32(data + off + 20 + i * 8);
                    }
                }
            }
        }
    }
    if (find_box(data, len, "stsc", &off, &sz)) {
        if (off + 12 <= len) {
            info->stsc_count = read_be32(data + off + 12);
            if (info->stsc_count > 0 && off + 16 + info->stsc_count * 12 <= len) {
                info->stsc_entries = (struct stsc_entry *)kmalloc(info->stsc_count * sizeof(struct stsc_entry));
                if (info->stsc_entries) {
                    for (uint32_t i = 0; i < info->stsc_count; i++) {
                        info->stsc_entries[i].first_chunk = read_be32(data + off + 16 + i * 12);
                        info->stsc_entries[i].samples_per_chunk = read_be32(data + off + 20 + i * 12);
                        info->stsc_entries[i].sample_desc_idx = read_be32(data + off + 24 + i * 12);
                    }
                }
            }
        }
    }
    return 1;
}

static int parse_trak(const uint8_t *data, size_t len,
                      struct mp4_info *info) {
    size_t off, sz;
    if (find_box(data, len, "mdia", &off, &sz)) {
        size_t mdia_off = off + 8, mdia_len = sz - 8;
        if (find_box(data + mdia_off, mdia_len, "hdlr", &off, &sz)) {
            if (off + 16 <= mdia_len) {
                const uint8_t *h = data + mdia_off + off + 8;
                if (h[4]=='v' && h[5]=='i' && h[6]=='d' && h[7]=='e') {
                    info->has_video = 1;
                    serial_print("[MP4] Found video track\n");
                }
            }
        }
        if (info->has_video) {
            if (find_box(data + mdia_off, mdia_len, "minf", &off, &sz)) {
                size_t minf_off = mdia_off + off + 8, minf_len = sz - 8;
                if (find_box(data + minf_off, minf_len, "stbl", &off, &sz)) {
                    parse_stbl(data + minf_off + off + 8, sz - 8, info);
                }
            }
        }
    }
    return info->has_video;
}

int mp4_parse(const char *path, struct mp4_info *info) {
    if (!path || !info) return -1;
    memset(info, 0, sizeof(*info));

    size_t fsize;
    void *fdata = vfs_read_file(path, &fsize);
    if (!fdata || fsize < 8) {
        serial_print("[MP4] Cannot read file\n");
        return -1;
    }

    serial_print("[MP4] File size: ");
    char sb[16]; int sp2=0; size_t sv2=fsize; if(sv2==0){sb[sp2++]='0';}else{char t[16];int tp=0;while(sv2>0){t[tp++]='0'+(sv2%10);sv2/=10;}while(tp)sb[sp2++]=t[--tp];} sb[sp2]='\n'; sb[sp2+1]=0;
    serial_print(sb);

    const uint8_t *data = (const uint8_t *)fdata;
    size_t off, sz;

    if (find_box(data, fsize, "ftyp", &off, &sz)) {
        serial_print("[MP4] ftyp found\n");
    } else {
        serial_print("[MP4] Not a valid MP4 (no ftyp)\n");
        kfree(fdata);
        return -1;
    }

    if (find_box(data, fsize, "moov", &off, &sz)) {
        serial_print("[MP4] moov found\n");
        size_t moov_off = off + 8, moov_len = sz - 8;

        size_t trak_off2, trak_sz;
        size_t pos = 0;
        while (find_box(data + moov_off + pos, moov_len - pos, "trak", &trak_off2, &trak_sz)) {
            serial_print("[MP4] trak found\n");
            if (parse_trak(data + moov_off + pos + trak_off2, trak_sz, info)) {
                break;
            }
            pos += trak_off2 + trak_sz;
        }

        if (!info->has_video) {
            serial_print("[MP4] No video track found\n");
        }
    } else {
        serial_print("[MP4] No moov box\n");
        kfree(fdata);
        return -1;
    }

    if (find_box(data, fsize, "mdat", &off, &sz)) {
        info->mdat_offset = off + 8;
        info->mdat_size = sz - 8;
        serial_print("[MP4] mdat found\n");
    }

    info->file_data = fdata;
    info->file_size = fsize;

    if (info->codec == MP4_CODEC_MJPEG && info->chunk_count > 0 && info->mdat_offset > 0) {
        info->valid = 1;
        serial_print("[MP4] Valid MJPEG MP4 ready for decode\n");
    } else if (info->codec == MP4_CODEC_H264) {
        serial_print("[MP4] H264 not supported, need MJPEG encoded MP4\n");
    }

    return info->valid ? 0 : -1;
}

static int get_sample_offset(struct mp4_info *info, uint32_t sample_idx,
                             uint64_t *offset, uint32_t *size) {
    if (!info->chunk_offsets || !info->stsc_entries) return -1;

    uint32_t sample_in_chunk = 0;
    uint32_t chunk_idx = 0;
    uint32_t acc = 0;

    for (uint32_t i = 0; i < info->stsc_count; i++) {
        uint32_t first = info->stsc_entries[i].first_chunk;
        uint32_t spc = info->stsc_entries[i].samples_per_chunk;
        uint32_t next_first = (i + 1 < info->stsc_count) ?
                              info->stsc_entries[i + 1].first_chunk :
                              info->chunk_count + 1;
        uint32_t chunks_in_run = next_first - first;
        uint32_t samples_in_run = chunks_in_run * spc;

        if (acc + samples_in_run > sample_idx) {
            uint32_t local = sample_idx - acc;
            chunk_idx = first - 1 + local / spc;
            sample_in_chunk = local % spc;
            break;
        }
        acc += samples_in_run;
    }

    if (chunk_idx >= info->chunk_count) return -1;
    *offset = info->chunk_offsets[chunk_idx];

    for (uint32_t j = 0; j < sample_in_chunk; j++) {
        uint32_t sidx = sample_idx - sample_in_chunk + j;
        if (info->sample_sizes) {
            *offset += info->sample_sizes[sidx];
        } else if (info->sample_size > 0) {
            *offset += info->sample_size;
        }
    }

    if (info->sample_sizes && sample_idx < info->sample_count) {
        *size = info->sample_sizes[sample_idx];
    } else if (info->sample_size > 0) {
        *size = info->sample_size;
    } else {
        return -1;
    }

    return 0;
}

int mp4_decode_frame(struct mp4_info *info, uint32_t frame_idx,
                     uint32_t *rgb_out, int out_w, int out_h) {
    if (!info || !info->valid || !info->file_data) return -1;
    if (frame_idx >= info->sample_count) return -1;
    if (info->codec != MP4_CODEC_MJPEG) return -1;

    uint64_t frame_off;
    uint32_t frame_size;
    if (get_sample_offset(info, frame_idx, &frame_off, &frame_size) != 0) {
        return -1;
    }
    if (frame_off + frame_size > info->file_size) return -1;

    const uint8_t *jpeg_data = (const uint8_t *)info->file_data + frame_off;
    return mjpeg_decode_frame(jpeg_data, frame_size, rgb_out, out_w, out_h);
}

void mp4_close(struct mp4_info *info) {
    if (!info) return;
    if (info->file_data) kfree(info->file_data);
    if (info->sample_sizes) kfree(info->sample_sizes);
    if (info->chunk_offsets) kfree(info->chunk_offsets);
    if (info->stsc_entries) kfree(info->stsc_entries);
    memset(info, 0, sizeof(*info));
}
