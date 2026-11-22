#include "mp4_decode.h"
#include "heap.h"
#include "string.h"
#include "console.h"

static uint16_t read_be16_j(const uint8_t *p) {
    return ((uint16_t)p[0]<<8)|p[1];
}

static const uint8_t jpeg_std_dht_l_bits[16] = {
    0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0
};
static const uint8_t jpeg_std_dht_l_vals[12] = {
    0,1,2,3,4,5,6,7,8,9,10,11
};
static const uint8_t jpeg_std_dht_c_bits[16] = {
    0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0
};
static const uint8_t jpeg_std_dht_c_vals[18] = {
    0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17
};

static const uint8_t zigzag[64] = {
    0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,
    12,19,26,33,40,48,41,34,27,20,13,6,7,14,
    21,28,35,42,49,56,57,50,43,36,29,22,15,
    23,30,37,44,51,58,59,52,45,38,31,39,46,
    53,60,61,54,47,55,62,63
};

static const int16_t idct_aan[64] = {
    16383, 22725, 21407, 19266, 16383, 12873,  8867,  4520,
    16383, 19266,  8867, -4520,-16383,-12873,-21407,-22725,
    16383, 12299,-12299,-16383,-16383,-12299, 12299, 16383,
    16383,  8867,-21407,-12873, 16383, 22725,-12299, -4520,
    16383,  4520,-21407, 12873, 16383,-22725,-12299,  8867,
    16383, -4520,-12299, 22725, 16383,-12873, 21407, -8867,
    16383, -8867, 12299, 22725,-16383,-12873, 21407,-4520,
    16383,-12299, 12299,-16383, 16383,-12299, 12299,-16383
};

struct jpeg_huff {
    uint16_t code[256];
    uint8_t len[256];
    uint8_t symbol_count;
    uint8_t symbols[256];
};

struct jpeg_state {
    const uint8_t *data;
    size_t size;
    size_t pos;
    int bit_buf;
    int bits_left;

    int width, height;
    int mcu_w, mcu_h;
    int h_max, v_max;
    int num_components;

    struct {
        int id;
        int h, v;
        int qt;
        int dc_huff, ac_huff;
        int dc_pred;
    } comp[4];

    int16_t qtable[4][64];
    struct jpeg_huff dc_huff[4];
    struct jpeg_huff ac_huff[4];
};

static int read_byte(struct jpeg_state *s) {
    if (s->pos >= s->size) return -1;
    return s->data[s->pos++];
}

static int read_word(struct jpeg_state *s) {
    int hi = read_byte(s);
    int lo = read_byte(s);
    if (hi < 0 || lo < 0) return -1;
    return (hi << 8) | lo;
}

static void align_bits(struct jpeg_state *s) {
    s->bits_left = 0;
    s->bit_buf = 0;
}

static int get_bit(struct jpeg_state *s) {
    if (s->bits_left == 0) {
        int b = read_byte(s);
        if (b < 0) return 0;
        if (b == 0xFF) {
            int nb = read_byte(s);
            if (nb < 0) return 0;
            if (nb == 0) b = 0xFF;
            else { s->pos -= 2; return 0; }
        }
        s->bit_buf = b;
        s->bits_left = 8;
    }
    s->bits_left--;
    int bit = (s->bit_buf >> s->bits_left) & 1;
    return bit;
}

static int get_bits(struct jpeg_state *s, int n) {
    int val = 0;
    for (int i = 0; i < n; i++) {
        val = (val << 1) | get_bit(s);
    }
    return val;
}

static int huff_decode(struct jpeg_state *s, struct jpeg_huff *h) {
    int code = 0;
    for (int len = 1; len <= 16; len++) {
        code = (code << 1) | get_bit(s);
        for (int i = 0; i < h->symbol_count; i++) {
            if (h->len[i] == len && h->code[i] == code) {
                return h->symbols[i];
            }
        }
    }
    return -1;
}

static int receive_extend(struct jpeg_state *s, int bits) {
    if (bits == 0) return 0;
    int val = get_bits(s, bits);
    if (val < (1 << (bits - 1))) {
        val += (-1 << bits) + 1;
    }
    return val;
}

static void build_huff_table(struct jpeg_huff *h, const uint8_t *bits,
                              const uint8_t *vals, int val_count) {
    h->symbol_count = 0;
    int code = 0;
    int idx = 0;
    for (int len = 1; len <= 16; len++) {
        for (int i = 0; i < bits[len-1] && idx < val_count; i++) {
            h->code[h->symbol_count] = code;
            h->len[h->symbol_count] = len;
            h->symbols[h->symbol_count] = vals[idx];
            h->symbol_count++;
            code++;
            idx++;
        }
        code <<= 1;
    }
}

static void parse_dht(struct jpeg_state *s, int len) {
    while (len > 17) {
        int b = read_byte(s);
        if (b < 0) return;
        len--;
        int cls = (b >> 4) & 1;
        int id = b & 0xF;
        if (id > 3) id = 3;

        uint8_t bits[16];
        int total = 0;
        for (int i = 0; i < 16; i++) {
            bits[i] = (uint8_t)read_byte(s);
            total += bits[i];
            len--;
        }
        if (total > 256) { while(len-- > 0) read_byte(s); return; }

        uint8_t vals[256];
        for (int i = 0; i < total; i++) {
            vals[i] = (uint8_t)read_byte(s);
            len--;
        }

        struct jpeg_huff *h = cls ? &s->ac_huff[id] : &s->dc_huff[id];
        build_huff_table(h, bits, vals, total);
    }
    while (len > 0) { read_byte(s); len--; }
}

static void parse_dqt(struct jpeg_state *s, int len) {
    while (len > 2) {
        int b = read_byte(s);
        if (b < 0) return;
        len--;
        int prec = (b >> 4) & 1;
        int id = b & 0xF;
        if (id > 3) id = 3;

        int count = prec ? 128 : 64;
        if (len < count) break;

        for (int i = 0; i < 64; i++) {
            if (prec) {
                int hi = read_byte(s);
                int lo = read_byte(s);
                s->qtable[id][zigzag[i]] = (int16_t)((hi << 8) | lo);
                len -= 2;
            } else {
                s->qtable[id][zigzag[i]] = (int16_t)read_byte(s);
                len--;
            }
        }
    }
    while (len > 0) { read_byte(s); len--; }
}

static void parse_sof(struct jpeg_state *s, int len) {
    int prec = read_byte(s);
    (void)prec;
    s->height = read_word(s);
    s->width = read_word(s);
    s->num_components = read_byte(s);
    len -= 6;

    for (int i = 0; i < s->num_components && len >= 3; i++) {
        s->comp[i].id = read_byte(s);
        int hv = read_byte(s);
        s->comp[i].h = (hv >> 4) & 0xF;
        s->comp[i].v = hv & 0xF;
        s->comp[i].qt = read_byte(s);
        s->comp[i].dc_pred = 0;
        len -= 3;
    }

    s->h_max = 1; s->v_max = 1;
    for (int i = 0; i < s->num_components; i++) {
        if (s->comp[i].h > s->h_max) s->h_max = s->comp[i].h;
        if (s->comp[i].v > s->v_max) s->v_max = s->comp[i].v;
    }
    s->mcu_w = s->h_max * 8;
    s->mcu_h = s->v_max * 8;

    while (len > 0) { read_byte(s); len--; }
}

static void parse_sos(struct jpeg_state *s, int len) {
    int ns = read_byte(s);
    len--;
    for (int i = 0; i < ns && len >= 2; i++) {
        int cs = read_byte(s);
        int td = read_byte(s);
        len -= 2;
        for (int j = 0; j < s->num_components; j++) {
            if (s->comp[j].id == cs) {
                s->comp[j].dc_huff = (td >> 4) & 0xF;
                s->comp[j].ac_huff = td & 0xF;
                break;
            }
        }
    }
    read_byte(s); read_byte(s); read_byte(s);
    len -= 3;
    while (len > 0) { read_byte(s); len--; }
    align_bits(s);
}

static void idct_block(int16_t *blk) {
    int tmp[64];
    for (int i = 0; i < 8; i++) {
        int x0 = blk[i*8+0], x1 = blk[i*8+4];
        int x2 = blk[i*8+2], x3 = blk[i*8+6];
        int x4 = blk[i*8+1], x5 = blk[i*8+7];
        int x6 = blk[i*8+3], x7 = blk[i*8+5];

        int a0 = x0 + x1, a1 = x0 - x1;
        int a2 = x2 - x3, a3 = x2 + x3;
        int b0 = 181*(x4+x5)+128>>8;
        int b1 = 181*(x4-x5)+128>>8;
        int b2 = 473*x6-160*x7+128>>8;
        int b3 = 160*x6+473*x7+128>>8;

        tmp[i*8+0] = a0+a3+b0;
        tmp[i*8+1] = a1+b2+b1;
        tmp[i*8+2] = a1-b2-b1;
        tmp[i*8+3] = a0-a3+b3;
        tmp[i*8+4] = a0-a3-b3;
        tmp[i*8+5] = a1-b2+b1;
        tmp[i*8+6] = a1+b2-b1;
        tmp[i*8+7] = a0+a3-b0;
    }
    for (int i = 0; i < 8; i++) {
        int x0 = tmp[0*8+i], x1 = tmp[4*8+i];
        int x2 = tmp[2*8+i], x3 = tmp[6*8+i];
        int x4 = tmp[1*8+i], x5 = tmp[7*8+i];
        int x6 = tmp[3*8+i], x7 = tmp[5*8+i];

        int a0 = x0+x1, a1 = x0-x1;
        int a2 = x2-x3, a3 = x2+x3;
        int b0 = 181*(x4+x5)+128>>8;
        int b1 = 181*(x4-x5)+128>>8;
        int b2 = 473*x6-160*x7+128>>8;
        int b3 = 160*x6+473*x7+128>>8;

        blk[0*8+i] = (a0+a3+b0+4)>>3;
        blk[1*8+i] = (a1+b2+b1+4)>>3;
        blk[2*8+i] = (a1-b2-b1+4)>>3;
        blk[3*8+i] = (a0-a3+b3+4)>>3;
        blk[4*8+i] = (a0-a3-b3+4)>>3;
        blk[5*8+i] = (a1-b2+b1+4)>>3;
        blk[6*8+i] = (a1+b2-b1+4)>>3;
        blk[7*8+i] = (a0+a3-b0+4)>>3;
    }
}

static int decode_block(struct jpeg_state *s, int comp_idx, int16_t *out) {
    int qt = s->comp[comp_idx].qt;
    struct jpeg_huff *dc = &s->dc_huff[s->comp[comp_idx].dc_huff];
    struct jpeg_huff *ac = &s->ac_huff[s->comp[comp_idx].ac_huff];

    memset(out, 0, 64 * sizeof(int16_t));

    int dc_sym = huff_decode(s, dc);
    if (dc_sym < 0) return -1;
    int dc_val = receive_extend(s, dc_sym);
    s->comp[comp_idx].dc_pred += dc_val;
    out[0] = s->comp[comp_idx].dc_pred * s->qtable[qt][0];

    for (int k = 1; k < 64; ) {
        int ac_sym = huff_decode(s, ac);
        if (ac_sym < 0) return -1;
        if (ac_sym == 0) break;
        if (ac_sym == 0xF0) { k += 16; continue; }
        int run = ac_sym >> 4;
        int bits = ac_sym & 0xF;
        k += run;
        if (k >= 64) return -1;
        int val = receive_extend(s, bits);
        out[zigzag[k]] = val * s->qtable[qt][zigzag[k]];
        k++;
    }

    idct_block(out);
    return 0;
}

static void clamp(int *v) {
    if (*v < 0) *v = 0;
    if (*v > 255) *v = 255;
}

static void ycbcr_to_rgb(int y, int cb, int cr, int *r, int *g, int *b) {
    *r = y + ((1436 * (cr - 128)) >> 10);
    *g = y - ((352 * (cb - 128) + 731 * (cr - 128)) >> 10);
    *b = y + ((1815 * (cb - 128)) >> 10);
    clamp(r); clamp(g); clamp(b);
}

int mjpeg_decode_frame(const uint8_t *data, size_t size,
                uint32_t *rgb_out, int out_w, int out_h) {
    if (!data || size < 4 || !rgb_out) return -1;
    if (data[0] != 0xFF || data[1] != 0xD8) return -1;

    struct jpeg_state s;
    memset(&s, 0, sizeof(s));
    s.data = data;
    s.size = size;
    s.pos = 0;

    for (int i = 0; i < 4; i++) {
        build_huff_table(&s.dc_huff[i], jpeg_std_dht_l_bits, jpeg_std_dht_l_vals, 12);
        build_huff_table(&s.ac_huff[i], jpeg_std_dht_c_bits, jpeg_std_dht_c_vals, 18);
    }

    int in_scan = 0;
    while (s.pos < s.size - 1) {
        int marker = read_byte(&s);
        if (marker != 0xFF) {
            if (in_scan) continue;
            continue;
        }
        marker = read_byte(&s);
        while (marker == 0xFF) marker = read_byte(&s);
        if (marker == 0) continue;

        in_scan = 0;

        if (marker == 0xD8) continue;
        if (marker == 0xD9) break;

        int seg_len = read_word(&s);
        if (seg_len < 2) break;
        seg_len -= 2;

        switch (marker) {
        case 0xDB: parse_dqt(&s, seg_len); break;
        case 0xC0: parse_sof(&s, seg_len); break;
        case 0xC4: parse_dht(&s, seg_len); break;
        case 0xDA:
            parse_sos(&s, seg_len);
            in_scan = 1;
            break;
        default:
            while (seg_len > 0 && s.pos < s.size) { read_byte(&s); seg_len--; }
            break;
        }
    }

    if (s.width <= 0 || s.height <= 0 || s.num_components < 1) return -1;

    int mcu_cols = (s.width + s.mcu_w - 1) / s.mcu_w;
    int mcu_rows = (s.height + s.mcu_h - 1) / s.mcu_h;

    s.pos = 0;
    while (s.pos < s.size - 1) {
        if (s.data[s.pos] == 0xFF && s.pos + 1 < s.size && s.data[s.pos+1] == 0xDA) {
            s.pos += 2;
            int sl = read_word(&s);
            s.pos += sl - 2;
            align_bits(&s);
            break;
        }
        s.pos++;
    }

    int16_t block[64];
    int16_t y_blocks[4][64], cb_block[64], cr_block[64];

    for (int mr = 0; mr < mcu_rows; mr++) {
        for (int mc = 0; mc < mcu_cols; mc++) {
            for (int c = 0; c < s.num_components; c++) {
                int blocks = s.comp[c].h * s.comp[c].v;
                for (int b = 0; b < blocks; b++) {
                    if (decode_block(&s, c, block) != 0) goto decode_done;

                    int bh = s.comp[c].h;
                    int bv = s.comp[c].v;
                    int bx = b % bh;
                    int by = b / bh;
                    int px = mc * s.mcu_w + bx * 8;
                    int py = mr * s.mcu_h + by * 8;

                    if (s.num_components == 1) {
                        for (int dy = 0; dy < 8; dy++) {
                            for (int dx = 0; dx < 8; dx++) {
                                int ox = px + dx, oy = py + dy;
                                if (ox < out_w && oy < out_h) {
                                    int v = block[dy*8+dx] + 128;
                                    clamp(&v);
                                    rgb_out[oy * out_w + ox] = ((uint32_t)v << 16) |
                                                                 ((uint32_t)v << 8) |
                                                                 (uint32_t)v;
                                }
                            }
                        }
                    } else {
                        if (c == 0) memcpy(y_blocks[b], block, 128);
                        else if (c == 1) memcpy(cb_block, block, 128);
                        else if (c == 2) memcpy(cr_block, block, 128);
                    }
                }
            }

            if (s.num_components == 3) {
                for (int dy = 0; dy < s.mcu_h && dy < 8 * s.v_max; dy++) {
                    for (int dx = 0; dx < s.mcu_w && dx < 8 * s.h_max; dx++) {
                        int ox = mc * s.mcu_w + dx;
                        int oy = mr * s.mcu_h + dy;
                        if (ox >= out_w || oy >= out_h) continue;

                        int yb = (dy / 8) * s.comp[0].h + (dx / 8);
                        if (yb >= 4) yb = 0;
                        int yi = (dy % 8) * 8 + (dx % 8);
                        int y_val = y_blocks[yb][yi];

                        int cbv = 128, crv = 128;
                        if (s.comp[1].h == s.comp[0].h && s.comp[1].v == s.comp[0].v) {
                            cbv = cb_block[(dy%8)*8+(dx%8)];
                            crv = cr_block[(dy%8)*8+(dx%8)];
                        } else {
                            cbv = cb_block[((dy/2)%8)*8+((dx/2)%8)];
                            crv = cr_block[((dy/2)%8)*8+((dx/2)%8)];
                        }

                        int r, g, b;
                        ycbcr_to_rgb(y_val, cbv, crv, &r, &g, &b);
                        rgb_out[oy * out_w + ox] = ((uint32_t)r << 16) |
                                                     ((uint32_t)g << 8) |
                                                     (uint32_t)b;
                    }
                }
            }
        }
    }

decode_done:
    return 0;
}
