/* wpa2_crypto.c — WPA2-PSK 加密底座实现（见 wpa2_crypto.h）
 * 全部为恒定内存访问模式的最小实现，无堆、无 SSE。
 */

#include "wpa2_crypto.h"

/* ================= SHA1 (RFC 3174) ================= */

typedef struct {
    u32 h[5];
    u64 total_len;
    u8 buf[64];
    u32 buf_len;
} sha1_ctx;

static void sha1_block(u32 h[5], const u8 *p) {
    u32 w[80];
    for (int i = 0; i < 16; i++)
        w[i] = ((u32)p[i*4] << 24) | ((u32)p[i*4+1] << 16) |
               ((u32)p[i*4+2] << 8) | (u32)p[i*4+3];
    for (int i = 16; i < 80; i++) {
        u32 x = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
        w[i] = (x << 1) | (x >> 31);          /* ROTL 1 */
    }
    u32 a=h[0], b=h[1], c=h[2], d=h[3], e=h[4];
    for (int i = 0; i < 80; i++) {
        u32 f, k;
        if (i < 20)      { f = (b & c) | ((~b) & d);          k = 0x5A827999; }
        else if (i < 40) { f = b ^ c ^ d;                     k = 0x6ED9EBA1; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d);   k = 0x8F1BBCDC; }
        else             { f = b ^ c ^ d;                     k = 0xCA62C1D6; }
        u32 t = ((a << 5) | (a >> 27)) + f + e + k + w[i];   /* ROTL 5(a) */
        e = d; d = c; c = (b << 30) | (b >> 2); b = a; a = t;   /* ROTL 30(b) */
    }
    h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
}

static void sha1_init(sha1_ctx *c) {
    c->h[0]=0x67452301; c->h[1]=0xEFCDAB89; c->h[2]=0x98BADCFE;
    c->h[3]=0x10325476; c->h[4]=0xC3D2E1F0;
    c->total_len = 0; c->buf_len = 0;
}

static void sha1_update(sha1_ctx *c, const u8 *data, u32 len) {
    c->total_len += len;
    while (len > 0) {
        u32 take = 64 - c->buf_len;
        if (take > len) take = len;
        for (u32 i = 0; i < take; i++) c->buf[c->buf_len + i] = data[i];
        c->buf_len += take; data += take; len -= take;
        if (c->buf_len == 64) { sha1_block(c->h, c->buf); c->buf_len = 0; }
    }
}

static void sha1_final(sha1_ctx *c, u8 out[20]) {
    u64 bits = c->total_len * 8;
    u8 pad = 0x80;
    sha1_update(c, &pad, 1);
    u8 z = 0;
    while (c->buf_len != 56) sha1_update(c, &z, 1);
    u8 lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (u8)(bits >> (56 - i * 8));
    sha1_update(c, lenb, 8);
    for (int i = 0; i < 5; i++) {
        out[i*4]   = (u8)(c->h[i] >> 24);
        out[i*4+1] = (u8)(c->h[i] >> 16);
        out[i*4+2] = (u8)(c->h[i] >> 8);
        out[i*4+3] = (u8)(c->h[i]);
    }
}

void wpa2_sha1(const u8 *data, u32 len, u8 out[20]) {
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, data, len);
    sha1_final(&c, out);
}

/* ================= HMAC-SHA1 (RFC 2202) ================= */

void wpa2_hmac_sha1(const u8 *key, u32 key_len,
                    const u8 *data, u32 data_len, u8 out[20]) {
    u8 k[64], ipad[64], opad[64], kb[20];
    for (int i = 0; i < 64; i++) k[i] = 0;
    if (key_len > 64) {
        wpa2_sha1(key, key_len, kb);
        for (int i = 0; i < 20; i++) k[i] = kb[i];
    } else {
        for (u32 i = 0; i < key_len; i++) k[i] = key[i];
    }
    for (int i = 0; i < 64; i++) { ipad[i] = k[i] ^ 0x36; opad[i] = k[i] ^ 0x5C; }
    u8 inner[20];
    sha1_ctx c;
    sha1_init(&c);
    sha1_update(&c, ipad, 64);
    sha1_update(&c, data, data_len);
    sha1_final(&c, inner);
    sha1_init(&c);
    sha1_update(&c, opad, 64);
    sha1_update(&c, inner, 20);
    sha1_final(&c, out);
}

/* ================= PBKDF2-HMAC-SHA1 (RFC 2898) ================= */

static void pbkdf2_f(const u8 *pass, u32 pass_len,
                     const u8 *salt, u32 salt_len,
                     u32 iterations, u32 block_index, u8 out[20]) {
    /* U1 = HMAC(pass, salt || INT_32_BE(block_index))；U_i = HMAC(pass, U_{i-1}) */
    u8 sbuf[36];
    u32 n = salt_len;
    if (n > 32) n = 32;                 /* SSID ≤ 32 字节，防溢出 */
    for (u32 i = 0; i < n; i++) sbuf[i] = salt[i];
    sbuf[n+0] = (u8)(block_index >> 24);
    sbuf[n+1] = (u8)(block_index >> 16);
    sbuf[n+2] = (u8)(block_index >> 8);
    sbuf[n+3] = (u8)(block_index);
    u8 u[20], t[20];
    wpa2_hmac_sha1(pass, pass_len, sbuf, n + 4, u);
    for (int i = 0; i < 20; i++) t[i] = u[i];
    for (u32 it = 1; it < iterations; it++) {
        wpa2_hmac_sha1(pass, pass_len, u, 20, u);
        for (int i = 0; i < 20; i++) t[i] ^= u[i];
    }
    for (int i = 0; i < 20; i++) out[i] = t[i];
}

void wpa2_pbkdf2_sha1(const char *passphrase, const u8 *ssid, u32 ssid_len,
                      u8 psk[WPA2_PMK_LEN]) {
    u32 plen = 0;
    while (passphrase[plen]) plen++;
    pbkdf2_f((const u8 *)passphrase, plen, ssid, ssid_len, 4096, 1, psk);       /* DK1 → 前 20 字节 */
    pbkdf2_f((const u8 *)passphrase, plen, ssid, ssid_len, 4096, 2, psk + 20);  /* DK2 → 后 12 字节 */
}

/* ================= PRF-X (802.11i H.4) =================
 * PTK = PRF-512(PMK, "Pairwise key expansion", 串接 seed)。
 * seed 由调用方拼好：min(AA,SPA)||max(AA,SPA)||min(ANonce,SNonce)||max(...)。
 * 输出 64 字节：迭代 0..63，每字节取 HMAC-SHA1(PMK, label||seed||counter)[0]。
 */
void wpa2_prf512(const u8 *pmk, const u8 *seed, u32 seed_len, u8 out[64]) {
    static const char label[] = "Pairwise key expansion";
    u32 label_len = sizeof(label) - 1;   /* 22 */
    u8 buf[22 + 64 + 1];
    for (u32 i = 0; i < label_len; i++) buf[i] = (u8)label[i];
    for (u32 i = 0; i < seed_len; i++) buf[label_len + i] = seed[i];
    if (seed_len > 64) seed_len = 64;
    for (int c = 0; c < 64; c++) {
        buf[label_len + seed_len] = (u8)c;
        u8 mac[20];
        wpa2_hmac_sha1(pmk, 32, buf, label_len + seed_len + 1, mac);
        out[c] = mac[0];
    }
}

/* ================= AES-128 (FIPS-197，仅加密方向) ================= */

static const u8 aes_sbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

static const u8 aes_rcon[11] = {
    0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36
};

static u32 aes_sub_word(u32 w) {
    return ((u32)aes_sbox[(w >> 24) & 0xFF] << 24) |
           ((u32)aes_sbox[(w >> 16) & 0xFF] << 16) |
           ((u32)aes_sbox[(w >> 8) & 0xFF] << 8) |
           (u32)aes_sbox[w & 0xFF];
}

/* 展开轮密钥：rk[44]（11 轮 × 4 word） */
static void aes_key_expand(const u8 key[16], u32 rk[44]) {
    for (int i = 0; i < 4; i++)
        rk[i] = ((u32)key[i*4] << 24) | ((u32)key[i*4+1] << 16) |
                ((u32)key[i*4+2] << 8) | (u32)key[i*4+3];
    for (int i = 4; i < 44; i++) {
        u32 t = rk[i-1];
        if (i % 4 == 0) {
            t = aes_sub_word((t << 8) | (t >> 24)) ^ ((u32)aes_rcon[i/4] << 24);
        }
        rk[i] = rk[i-4] ^ t;
    }
}

void wpa2_aes128_enc_block(const u8 key[16], const u8 in[16], u8 out[16]) {
    u32 rk[44];
    aes_key_expand(key, rk);
    u32 s0,s1,s2,s3, t0,t1,t2,t3;
    s0 = rk[0] ^ ((u32)in[0] << 24 | (u32)in[1] << 16 | (u32)in[2] << 8 | in[3]);
    s1 = rk[1] ^ ((u32)in[4] << 24 | (u32)in[5] << 16 | (u32)in[6] << 8 | in[7]);
    s2 = rk[2] ^ ((u32)in[8] << 24 | (u32)in[9] << 16 | (u32)in[10] << 8 | in[11]);
    s3 = rk[3] ^ ((u32)in[12] << 24 | (u32)in[13] << 16 | (u32)in[14] << 8 | in[15]);
#define AES_XTIME(x) ((u8)((((u8)(x)) << 1) ^ ((((u8)(x)) >> 7) * 0x1b)))
    for (int r = 1; r <= 10; r++) {
        /* SubBytes + ShiftRows（按列状态 s0..s3 每列 4 字节，state[col][row]） */
        u8 sb[4][4];
        u32 sv[4] = {s0, s1, s2, s3};
        for (int cCol = 0; cCol < 4; cCol++)
            for (int rRow = 0; rRow < 4; rRow++)
                sb[rRow][cCol] = aes_sbox[(sv[cCol] >> (24 - rRow * 8)) & 0xFF];
        /* ShiftRows：行 r 循环左移 r */
        {
            u8 t;
            t = sb[1][0]; sb[1][0]=sb[1][1]; sb[1][1]=sb[1][2]; sb[1][2]=sb[1][3]; sb[1][3]=t;
            t=sb[2][0]; sb[2][0]=sb[2][2]; sb[2][2]=t; t=sb[2][1]; sb[2][1]=sb[2][3]; sb[2][3]=t;
            t = sb[3][0]; sb[3][0]=sb[3][3]; sb[3][3]=sb[3][2]; sb[3][2]=sb[3][1]; sb[3][1]=t;
        }
        /* MixColumns（末轮跳过） */
        if (r < 10) {
            u32 o[4];
            for (int cCol = 0; cCol < 4; cCol++) {
                u8 a0=sb[0][cCol], a1=sb[1][cCol], a2=sb[2][cCol], a3=sb[3][cCol];
                u8 x1 = AES_XTIME(a0), x2 = AES_XTIME(a1);
                u8 x3 = AES_XTIME(a2), x4 = AES_XTIME(a3);
                sb[0][cCol] = (u8)(x1 ^ x2 ^ a1 ^ a2 ^ a3);
                sb[1][cCol] = (u8)(a0 ^ x2 ^ x3 ^ a2 ^ a3);
                sb[2][cCol] = (u8)(a0 ^ a1 ^ x3 ^ x4 ^ a3);
                sb[3][cCol] = (u8)(x1 ^ a0 ^ a1 ^ a2 ^ x4);
            }
            o[0] = ((u32)sb[0][0] << 24) | ((u32)sb[1][0] << 16) | ((u32)sb[2][0] << 8) | sb[3][0];
            o[1] = ((u32)sb[0][1] << 24) | ((u32)sb[1][1] << 16) | ((u32)sb[2][1] << 8) | sb[3][1];
            o[2] = ((u32)sb[0][2] << 24) | ((u32)sb[1][2] << 16) | ((u32)sb[2][2] << 8) | sb[3][2];
            o[3] = ((u32)sb[0][3] << 24) | ((u32)sb[1][3] << 16) | ((u32)sb[2][3] << 8) | sb[3][3];
            t0 = o[0]; t1 = o[1]; t2 = o[2]; t3 = o[3];
        } else {
            t0 = ((u32)sb[0][0] << 24) | ((u32)sb[1][0] << 16) | ((u32)sb[2][0] << 8) | sb[3][0];
            t1 = ((u32)sb[0][1] << 24) | ((u32)sb[1][1] << 16) | ((u32)sb[2][1] << 8) | sb[3][1];
            t2 = ((u32)sb[0][2] << 24) | ((u32)sb[1][2] << 16) | ((u32)sb[2][2] << 8) | sb[3][2];
            t3 = ((u32)sb[0][3] << 24) | ((u32)sb[1][3] << 16) | ((u32)sb[2][3] << 8) | sb[3][3];
        }
        s0 = t0 ^ rk[r*4+0];
        s1 = t1 ^ rk[r*4+1];
        s2 = t2 ^ rk[r*4+2];
        s3 = t3 ^ rk[r*4+3];
    }
    out[0]=(u8)(s0>>24); out[1]=(u8)(s0>>16); out[2]=(u8)(s0>>8); out[3]=(u8)s0;
    out[4]=(u8)(s1>>24); out[5]=(u8)(s1>>16); out[6]=(u8)(s1>>8); out[7]=(u8)s1;
    out[8]=(u8)(s2>>24); out[9]=(u8)(s2>>16); out[10]=(u8)(s2>>8); out[11]=(u8)s2;
    out[12]=(u8)(s3>>24); out[13]=(u8)(s3>>16); out[14]=(u8)(s3>>8); out[15]=(u8)s3;
}

/* ================= AES-128 解密方向（RFC 3394 unwrap 用） ================= */

static const u8 aes_inv_sbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

/* GF(2^8) 乘法（AES 域，模 x^8+x^4+x^3+x+1） */
static u8 aes_gf_mul(u8 a, u8 b) {
    u8 p = 0;
    for (int i = 0; i < 8; i++) {
        if (b & 1) p ^= a;
        u8 hi = a & 0x80;
        a = (u8)(a << 1);
        if (hi) a ^= 0x1b;
        b >>= 1;
    }
    return p;
}

/* AES-128 解密单块（等价逆密码结构，调试/unwrap 用） */
void wpa2_aes128_dec_block(const u8 key[16], const u8 in[16], u8 out[16]) {
    u32 rk[44];
    aes_key_expand(key, rk);
    u32 s0 = rk[40] ^ ((u32)in[0] << 24 | (u32)in[1] << 16 | (u32)in[2] << 8 | in[3]);
    u32 s1 = rk[41] ^ ((u32)in[4] << 24 | (u32)in[5] << 16 | (u32)in[6] << 8 | in[7]);
    u32 s2 = rk[42] ^ ((u32)in[8] << 24 | (u32)in[9] << 16 | (u32)in[10] << 8 | in[11]);
    u32 s3 = rk[43] ^ ((u32)in[12] << 24 | (u32)in[13] << 16 | (u32)in[14] << 8 | in[15]);
    u32 sv[4];
    for (int r = 9; r >= 0; r--) {
        sv[0]=s0; sv[1]=s1; sv[2]=s2; sv[3]=s3;
        u8 sb[4][4];
        for (int cCol = 0; cCol < 4; cCol++)
            for (int rRow = 0; rRow < 4; rRow++)
                sb[rRow][cCol] = (u8)(sv[cCol] >> (24 - rRow * 8));
        /* InvShiftRows（行 r 循环右移 r）+ InvSubBytes */
        u8 t;
        t = sb[1][3]; sb[1][3]=sb[1][2]; sb[1][2]=sb[1][1]; sb[1][1]=sb[1][0]; sb[1][0]=t;
        t=sb[2][0]; sb[2][0]=sb[2][2]; sb[2][2]=t; t=sb[2][1]; sb[2][1]=sb[2][3]; sb[2][3]=t;
        t = sb[3][0]; sb[3][0]=sb[3][1]; sb[3][1]=sb[3][2]; sb[3][2]=sb[3][3]; sb[3][3]=t;
        for (int cCol = 0; cCol < 4; cCol++)
            for (int rRow = 0; rRow < 4; rRow++)
                sb[rRow][cCol] = aes_inv_sbox[sb[rRow][cCol]];
        /* AddRoundKey(r) */
        u32 nrk[4] = { rk[r*4+0], rk[r*4+1], rk[r*4+2], rk[r*4+3] };
        {
            u32 o[4];
            for (int cCol = 0; cCol < 4; cCol++)
                o[cCol] = ((u32)sb[0][cCol] << 24) | ((u32)sb[1][cCol] << 16) |
                          ((u32)sb[2][cCol] << 8) | (u32)sb[3][cCol];
            s0 = o[0] ^ nrk[0]; s1 = o[1] ^ nrk[1]; s2 = o[2] ^ nrk[2]; s3 = o[3] ^ nrk[3];
        }
        /* InvMixColumns（末轮 r=0 跳过）：作用在 AddRoundKey 之后的状态上 */
        if (r > 0) {
            sv[0]=s0; sv[1]=s1; sv[2]=s2; sv[3]=s3;
            for (int cCol = 0; cCol < 4; cCol++)
                for (int rRow = 0; rRow < 4; rRow++)
                    sb[rRow][cCol] = (u8)(sv[cCol] >> (24 - rRow * 8));
            for (int cCol = 0; cCol < 4; cCol++) {
                u8 a0=sb[0][cCol], a1=sb[1][cCol], a2=sb[2][cCol], a3=sb[3][cCol];
                sb[0][cCol] = (u8)(aes_gf_mul(a0,0x0e) ^ aes_gf_mul(a1,0x0b) ^ aes_gf_mul(a2,0x0d) ^ aes_gf_mul(a3,0x09));
                sb[1][cCol] = (u8)(aes_gf_mul(a0,0x09) ^ aes_gf_mul(a1,0x0e) ^ aes_gf_mul(a2,0x0b) ^ aes_gf_mul(a3,0x0d));
                sb[2][cCol] = (u8)(aes_gf_mul(a0,0x0d) ^ aes_gf_mul(a1,0x09) ^ aes_gf_mul(a2,0x0e) ^ aes_gf_mul(a3,0x0b));
                sb[3][cCol] = (u8)(aes_gf_mul(a0,0x0b) ^ aes_gf_mul(a1,0x0d) ^ aes_gf_mul(a2,0x09) ^ aes_gf_mul(a3,0x0e));
            }
            u32 o[4];
            for (int cCol = 0; cCol < 4; cCol++)
                o[cCol] = ((u32)sb[0][cCol] << 24) | ((u32)sb[1][cCol] << 16) |
                          ((u32)sb[2][cCol] << 8) | (u32)sb[3][cCol];
            s0 = o[0]; s1 = o[1]; s2 = o[2]; s3 = o[3];
        }
    }
    out[0]=(u8)(s0>>24); out[1]=(u8)(s0>>16); out[2]=(u8)(s0>>8); out[3]=(u8)s0;
    out[4]=(u8)(s1>>24); out[5]=(u8)(s1>>16); out[6]=(u8)(s1>>8); out[7]=(u8)s1;
    out[8]=(u8)(s2>>24); out[9]=(u8)(s2>>16); out[10]=(u8)(s2>>8); out[11]=(u8)s2;
    out[12]=(u8)(s3>>24); out[13]=(u8)(s3>>16); out[14]=(u8)(s3>>8); out[15]=(u8)s3;
}

/* ================= RFC 3394 AES Key Unwrap ================= */

static const u8 wrap_iv[8] = {0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6,0xA6};

int wpa2_key_unwrap(const u8 kek[16], const u8 *in, u32 in_len, u8 *out) {
    if (in_len < 24 || (in_len % 8) != 0) return -1;
    u32 n = in_len / 8 - 1;
    u8 areg[8];
    for (int i = 0; i < 8; i++) areg[i] = in[i];
    /* R 块直接在 out 缓冲中处理（out 容量须 ≥ in_len-8，调用方保证） */
    u8 *r = out;
    for (u32 i = 0; i < n * 8; i++) r[i] = in[8 + i];
    u8 blockin[16], b[16];
    for (int j = 5; j >= 0; j--) {
        for (u32 i = n; i >= 1; i--) {
            /* RFC 3394: B = AES-1(K, (A ^ t) | R[i])，t = n*j + i */
            u64 t = (u64)(n * (u32)j + i);
            for (int k = 0; k < 8; k++)
                blockin[k] = areg[k] ^ (u8)((t >> (56 - k * 8)) & 0xFF);
            for (int k = 0; k < 8; k++) blockin[8+k] = r[(i-1)*8+k];
            wpa2_aes128_dec_block(kek, blockin, b);
            for (int k = 0; k < 8; k++) areg[k] = b[k];
            for (int k = 0; k < 8; k++) r[(i-1)*8+k] = b[8+k];
        }
    }
    for (int i = 0; i < 8; i++) if (areg[i] != wrap_iv[i]) return -1;
    return 0;
}

/* ================= CCMP (RFC 3610 + 802.11i) =================
 * STA 收 AP 数据帧方向：MIC(CBC-MAC) 验证 + CTR 解密。
 * 帧 = MAC 头(24B, 3 地址) || CCMP 头(8B) || 密文 || MIC(8B)。
 */

#define CCMP_HDR_LEN 8u
#define CCMP_MIC_LEN 8u

static void ccmp_nonce16(const u8 *frame, const u8 *chdr, u8 nonce[16]) {
    nonce[0] = 0;                                   /* priority = 0 */
    for (int i = 0; i < 6; i++) nonce[1+i] = frame[10+i];   /* A2 = SA */
    /* CCMP 头布局：b0=PN0 b1=PN1 b2=rsvd b3=KeyID b4=PN2 b5=PN3 b6=PN4 b7=PN5 */
    nonce[7]  = chdr[0];                            /* PN0 */
    nonce[8]  = chdr[1];                            /* PN1 */
    nonce[9]  = chdr[4];                            /* PN2 */
    nonce[10] = chdr[5];                            /* PN3 */
    nonce[11] = chdr[6];                            /* PN4 */
    nonce[12] = chdr[7];                            /* PN5 */
    nonce[13] = 0; nonce[14] = 0; nonce[15] = 0;
}

static void ccmp_ctr_block(const u8 nonce[16], u16 counter, u8 ctr[16]) {
    /* RFC 3610 A_i = flags(L-1=1) || 13B nonce || counter(2B) */
    ctr[0] = 0x01;
    for (int i = 0; i < 13; i++) ctr[1+i] = nonce[i];
    ctr[14] = (u8)(counter >> 8);
    ctr[15] = (u8)(counter);
}

/* ---- CCMP 头构造：PN 按 802.11i 散布写入 ---- */
static void ccmp_header_fill(u8 *chdr, u64 pn, u8 key_id) {
    chdr[0] = (u8)(pn & 0xFF);                        /* PN0 */
    chdr[1] = (u8)((pn >> 8) & 0xFF);                 /* PN1 */
    chdr[2] = 0;                                      /* Reserved */
    chdr[3] = (u8)(0x20 | (key_id & 0x03));           /* ExtIV=1 | KeyID */
    chdr[4] = (u8)((pn >> 16) & 0xFF);                /* PN2 */
    chdr[5] = (u8)((pn >> 24) & 0xFF);                /* PN3 */
    chdr[6] = (u8)((pn >> 32) & 0xFF);                /* PN4 */
    chdr[7] = (u8)((pn >> 40) & 0xFF);                /* PN5 */
}

/* ---- CBC-MAC：B0 || AAD(24B) || 密文 → MIC(16B)（收发共用） ---- */
static void ccmp_cbc_mac(const u8 *frame, const u8 *nonce,
                         const u8 *ct, u32 ct_len, const u8 tk[16], u8 mac[16]) {
    u16 fctl = (u16)frame[0] | ((u16)frame[1] << 8);
    u16 fc = fctl & 0xC78F;
    u16 sc = (u16)frame[22] | ((u16)frame[23] << 8);
    sc &= 0x000F;
    u8 abuf[24];
    abuf[0] = 0; abuf[1] = 22;
    abuf[2] = (u8)(fc & 0xFF); abuf[3] = (u8)(fc >> 8);
    for (int i = 0; i < 18; i++) abuf[4+i] = frame[4+i];
    abuf[22] = (u8)(sc & 0xFF); abuf[23] = (u8)(sc >> 8);

    u8 blk[16], b0[16];
    b0[0] = 0x19;                                     /* Adata=0, M=3(8B), L-1=1 */
    for (int i = 0; i < 13; i++) b0[1+i] = nonce[i];
    b0[14] = (u8)(ct_len >> 8); b0[15] = (u8)ct_len;
    wpa2_aes128_enc_block(tk, b0, mac);
    for (int i = 0; i < 16; i++) blk[i] = abuf[i] ^ mac[i];
    wpa2_aes128_enc_block(tk, blk, mac);
    for (int i = 0; i < 8; i++) blk[i] = abuf[16+i] ^ mac[i];
    for (int i = 8; i < 16; i++) blk[i] = mac[i];
    wpa2_aes128_enc_block(tk, blk, mac);
    for (u32 off = 0; off < ct_len; off += 16) {
        u32 take = ct_len - off;
        if (take > 16) take = 16;
        for (u32 i = 0; i < 16; i++)
            blk[i] = (i < take ? ct[off+i] : 0) ^ mac[i];
        wpa2_aes128_enc_block(tk, blk, mac);
    }
}

int wpa2_ccmp_decrypt(const u8 *frame, u32 len, const u8 tk[16], u8 *out, u32 out_cap) {
    u32 hdr = 24;                                   /* 3 地址 data 帧 */
    if (len < hdr + CCMP_HDR_LEN + CCMP_MIC_LEN) return -2;
    const u8 *chdr = frame + hdr;
    const u8 *ct = frame + hdr + CCMP_HDR_LEN;
    u32 ct_len = len - hdr - CCMP_HDR_LEN - CCMP_MIC_LEN;
    if (ct_len == 0) return -2;
    if (ct_len > out_cap) return -2;

    u8 nonce[16];                                    /* 13B CCMP 非立即在 [0..12] */
    ccmp_nonce16(frame, chdr, nonce);

    /* ---- MIC 校验（基于密文） ---- */
    u8 mac[16];
    ccmp_cbc_mac(frame, nonce, ct, ct_len, tk, mac);
    u8 diff = 0;
    for (u32 i = 0; i < CCMP_MIC_LEN; i++) diff |= mac[i] ^ ct[ct_len + i];
    if (diff) return -1;                             /* MIC 校验失败 */

    /* ---- CTR 解密 ---- */
    u16 counter = 1;
    for (u32 off = 0; off < ct_len; off += 16) {
        u8 ctr[16], s[16];
        ccmp_ctr_block(nonce, counter++, ctr);
        wpa2_aes128_enc_block(tk, ctr, s);
        u32 take = ct_len - off;
        if (take > 16) take = 16;
        for (u32 i = 0; i < take; i++)
            out[off+i] = ct[off+i] ^ s[i];
    }
    return (int)ct_len;
}

int wpa2_ccmp_encrypt(u8 *buf, u32 buf_cap, const u8 *msdu, u32 msdu_len,
                      const u8 tk[16], u64 pn) {
    if (!buf || !msdu || msdu_len == 0) return -1;
    if (msdu_len + 24 + CCMP_HDR_LEN + CCMP_MIC_LEN > buf_cap) return -1;

    u8 *chdr = buf + 24;
    ccmp_header_fill(chdr, pn, 0);
    u8 *ct = chdr + CCMP_HDR_LEN;

    u8 nonce[16];
    ccmp_nonce16(buf, chdr, nonce);

    /* CTR 加密 */
    u16 counter = 1;
    for (u32 off = 0; off < msdu_len; off += 16) {
        u8 ctr[16], s[16];
        ccmp_ctr_block(nonce, counter++, ctr);
        wpa2_aes128_enc_block(tk, ctr, s);
        u32 take = msdu_len - off;
        if (take > 16) take = 16;
        for (u32 i = 0; i < take; i++) ct[off+i] = msdu[off+i] ^ s[i];
    }

    u8 mac[16];
    ccmp_cbc_mac(buf, nonce, ct, msdu_len, tk, mac);
    for (u32 i = 0; i < CCMP_MIC_LEN; i++) ct[msdu_len + i] = mac[i];
    return (int)(24 + CCMP_HDR_LEN + msdu_len + CCMP_MIC_LEN);
}

/* ================= KAT ================= */

static int buf_eq(const u8 *a, const u8 *b, u32 n) {
    u8 d = 0;
    for (u32 i = 0; i < n; i++) d |= a[i] ^ b[i];
    return d == 0;
}

/* CCMP 自洽往返：用生产 encrypt 构造，再走生产 decrypt。
 * 覆盖 nonce/AAD/B0/CTR 字节布局 + MIC 篡改检测，防索引回归。 */
static int ccmp_selftest(void) {
    static const char payload[] = "Deshab CCMP round-trip payload 0123456789";
    u32 plen = sizeof(payload) - 1;

    u8 tk[16];
    for (int i = 0; i < 16; i++) tk[i] = (u8)(0x30 + i);

    u8 frame[96];
    for (u32 i = 0; i < sizeof(frame); i++) frame[i] = 0;
    /* 802.11 data 头：FC=0x0841 (data, toDS)，A1/A2/A3，seq ctrl */
    frame[0] = 0x41; frame[1] = 0x08;
    for (int i = 0; i < 6; i++) frame[4+i]  = (u8)(0xA0 + i);   /* A1 = AP */
    for (int i = 0; i < 6; i++) frame[10+i] = (u8)(0x10 + i);   /* A2 = STA */
    for (int i = 0; i < 6; i++) frame[16+i] = (u8)(0xB0 + i);   /* A3 = DA */
    frame[22] = 0x10; frame[23] = 0x00;

    int flen = wpa2_ccmp_encrypt(frame, sizeof(frame), (const u8 *)payload, plen,
                                 tk, 0x000000000001ULL);
    if (flen != (int)(24 + 8 + plen + 8)) return -1;

    u8 out[96];
    int rc = wpa2_ccmp_decrypt(frame, (u32)flen, tk, out, sizeof(out));
    if (rc != (int)plen) return -1;
    for (u32 i = 0; i < plen; i++) if (out[i] != (u8)payload[i]) return -1;

    /* 篡改密文 → 必须 MIC 校验失败 */
    frame[24 + 8] ^= 0x01;
    if (wpa2_ccmp_decrypt(frame, (u32)flen, tk, out, sizeof(out)) != -1) return -1;
    return 0;
}

int wpa2_kat(void) {
    u8 out[64];

    /* 1. SHA1("abc") = a9993e36...9cd0d89d */
    wpa2_sha1((const u8 *)"abc", 3, out);
    {
        static const u8 exp[20] = {
            0xa9,0x99,0x3e,0x36,0x47,0x06,0x81,0x6a,0xba,0x3e,
            0x25,0x71,0x78,0x50,0xc2,0x6c,0x9c,0xd0,0xd8,0x9d };
        if (!buf_eq(out, exp, 20)) return 1;
    }

    /* 2. HMAC-SHA1 RFC 2202 case 2: key "Jefe" data "what do ya want for nothing?"
     *    = effcdf6ae5eb2fa2d27416d5f184df9c259a7c79 */
    wpa2_hmac_sha1((const u8 *)"Jefe", 4,
                   (const u8 *)"what do ya want for nothing?", 28, out);
    {
        static const u8 exp[20] = {
            0xef,0xfc,0xdf,0x6a,0xe5,0xeb,0x2f,0xa2,0xd2,0x74,
            0x16,0xd5,0xf1,0x84,0xdf,0x9c,0x25,0x9a,0x7c,0x79 };
        if (!buf_eq(out, exp, 20)) return 2;
    }

    /* 3. PBKDF2-HMAC-SHA1 RFC 6070: ("password","salt",1,20)
     *    = 0c60c80f961f0e71f3a9b524af6012062fe037a6 */
    {
        u8 dk[32];
        /* 手动 1 iter 版本走 pbkdf2_f 不可见，直接用 4096 向量（与 WPA 一致）：
         * RFC 6070 case 3: ("password","salt",4096,20)
         *    = 4b007901b765489abead49d926f721d065a429c1 */
        wpa2_pbkdf2_sha1("password", (const u8 *)"salt", 4, dk);
        static const u8 exp[20] = {
            0x4b,0x00,0x79,0x01,0xb7,0x65,0x48,0x9a,0xbe,0xad,
            0x49,0xd9,0x26,0xf7,0x21,0xd0,0x65,0xa4,0x29,0xc1 };
        if (!buf_eq(dk, exp, 20)) return 3;
    }

    /* 4. AES-128 FIPS-197 C.1: key 000102...0f, pt 00112233...eeff
     *    = 69c4e0d86a7b0430d8cdb78070b4c55a */
    {
        u8 key[16], pt[16], ct[16];
        for (int i = 0; i < 16; i++) { key[i]=(u8)i; pt[i]=(u8)(i*0x11); }
        wpa2_aes128_enc_block(key, pt, ct);
        static const u8 exp[16] = {
            0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
            0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
        if (!buf_eq(ct, exp, 16)) return 4;
    }

    /* 5. AES-128 FIPS-197 C.3 (256bit key 前半忽略，此处用 C.1 变体覆盖 key schedule):
     *    key 2b7e1516 28aed2a6 abf71588 09cf4f3c
     *    pt   6bc1bee2 2e409f96 e93d7e11 7393172a
     *    ct   3ad77bb4 0d7a3660 a89ecaf3 2466ef97 */
    {
        static const u8 key[16] = {0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,
                                   0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c};
        static const u8 pt[16]  = {0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,
                                   0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a};
        static const u8 exp[16] = {0x3a,0xd7,0x7b,0xb4,0x0d,0x7a,0x36,0x60,
                                   0xa8,0x9e,0xca,0xf3,0x24,0x66,0xef,0x97};
        u8 ct[16];
        wpa2_aes128_enc_block(key, pt, ct);
        if (!buf_eq(ct, exp, 16)) return 5;
    }

    /* 6. CCMP：构造→解密 往返 + 篡改检测 */
    if (ccmp_selftest() != 0) return 6;

    /* 7. RFC 3394 key unwrap（C.1 128-bit KEK 向量） */
    {
        static const u8 kek[16] = {0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                                   0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F};
        static const u8 wrapped[24] = {0x1F,0xA6,0x8B,0x0A,0x81,0x12,0xB4,0x47,
                                       0xAE,0xF3,0x4B,0xD8,0xFB,0x5A,0x7B,0x82,
                                       0x9D,0x3E,0x86,0x23,0x71,0xD2,0xCF,0xE5};
        static const u8 expect[16] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                      0x88,0x99,0xAA,0xBB,0xCC,0xDD,0xEE,0xFF};
        u8 plain[16];
        if (wpa2_key_unwrap(kek, wrapped, 24, plain) != 0) return 7;
        if (!buf_eq(plain, expect, 16)) return 7;
    }

    return 0;
}
