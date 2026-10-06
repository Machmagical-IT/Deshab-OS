/* wpa2_crypto.h — WPA2-PSK 握手所需加密底座（DKM ath9k 专用）
 *
 * 范围（802.11i RSNA，AKM 00-0F-AC:2 PSK）：
 *   SHA1        — RFC 3174
 *   HMAC-SHA1   — RFC 2204（EAPOL-Key MIC ver2 = HMAC-SHA1-128）
 *   PBKDF2-HMAC-SHA1 — RFC 2898（passphrase → PSK，4096 iter）
 *   PRF-X       — 802.11i H.4（PMK → 512bit PTK）
 *   AES-128     — FIPS-197（仅加密方向；CCMP 的 CTR 与 CBC-MAC 都用 AES-ENC）
 *   AES Key Unwrap — RFC 3394（KEK 解密 M3 key data 中的 GTK KDE）
 *   CCMP        — RFC 3610（data 帧 MIC 验证 + 计数器解密；加密留 TX 数据路径）
 *
 * 自检：wpa2_kat() 内置 RFC 2202/6070/FIPS-197 已知答案向量，
 *       driver_init 时调用，任一失败 panic（ATH-E02）。
 * 约束：无堆（静态/栈缓冲）、无除法热路径、-mno-sse 可编译。
 */
#ifndef ATH9K_WPA2_CRYPTO_H
#define ATH9K_WPA2_CRYPTO_H

#include "../dkm_shared.h"

#define WPA2_PMK_LEN 32
#define WPA2_PTK_LEN 48     /* KCK(16)+KEK(16)+TK(16)（802.11i CCMP 不取 TKIP MIC 子钥） */
#define WPA2_KCK_LEN 16
#define WPA2_KEK_LEN 16
#define WPA2_TK_LEN  16

/* SHA1：out[20] */
void wpa2_sha1(const u8 *data, u32 len, u8 out[20]);
/* 增量 SHA1（EAPOL MIC 需要对任意长帧做 HMAC，直接给一次性接口） */
void wpa2_hmac_sha1(const u8 *key, u32 key_len,
                    const u8 *data, u32 data_len, u8 out[20]);
/* PBKDF2-HMAC-SHA1：passphrase → psk[32]（iter 固定 4096，salt=SSID） */
void wpa2_pbkdf2_sha1(const char *passphrase, const u8 *ssid, u32 ssid_len,
                      u8 psk[WPA2_PMK_LEN]);
/* PRF-512（802.11i H.4）：PMK+label+addr/nonce 串接 → out[64] */
void wpa2_prf512(const u8 *pmk, const u8 *seed, u32 seed_len, u8 out[64]);

/* AES-128 单块加密（in/out 16 字节，可同址） */
void wpa2_aes128_enc_block(const u8 key[16], const u8 in[16], u8 out[16]);
/* AES-128 单块解密（RFC 3394 unwrap 用；CCMP 路径不使用） */
void wpa2_aes128_dec_block(const u8 key[16], const u8 in[16], u8 out[16]);

/* RFC 3394 key unwrap：in 为 n*8 字节密文（n>=2），out 得 (n-1)*8 明文；
 * 返回 0 成功，-1 integrity check 失败（AIV != 0xA6A6A6A6A6A6A6A6）。 */
int wpa2_key_unwrap(const u8 kek[16], const u8 *in, u32 in_len, u8 *out);

/* CCMP 解密（STA 收 AP 数据帧，非 QoS，3 地址）：
 * frame 指向 802.11 MAC 头起始，len 为整帧长度（含 CCMP 8B 头 + 8B MIC）。
 * 成功时把解密后的数据载荷（去掉 CCMP 头/MIC/FCS 前的明文 MSDU）
 * 写入 out 并返回明文长度；MIC 错误返回 -1，参数错返回 -2。 */
int wpa2_ccmp_decrypt(const u8 *frame, u32 len, const u8 tk[16], u8 *out, u32 out_cap);

/* CCMP 加密（STA 发 AP 数据帧）：在 buf 内就地构造
 * [MAC 头(24B，调用方先填好)][CCMP 头(8B)][密文][MIC(8B)]。
 *   buf     : 调用方提供的缓冲，前 24 字节须已填 MAC 头
 *   msdu    : 明文 MSDU
 *   msdu_len: 明文长度（>0）
 *   pn      : 发包序号（调用方递增维护）
 * 返回写入 buf 的总帧长（24+8+msdu_len+8）；缓冲不足返回 -1。 */
int wpa2_ccmp_encrypt(u8 *buf, u32 buf_cap, const u8 *msdu, u32 msdu_len,
                      const u8 tk[16], u64 pn);

/* KAT（已知答案自检）：返回 0 = 全部通过；非 0 = 第几个用例失败（1 基）。
 * driver_init 必须调用，失败方 panic（ATH-E02）。 */
int wpa2_kat(void);

#endif
