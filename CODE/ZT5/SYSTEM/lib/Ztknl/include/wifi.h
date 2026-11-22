/*
 * WiFi Subsystem for Ztknl Kernel
 *
 * Supports:
 * - 802.11b/g/n (2.4 GHz)
 * - WPA/WPA2-PSK authentication
 * - USB WiFi adapters (rtl8188eu, rtl8192eu, etc.)
 * - Managed (station) mode
 */

#ifndef WIFI_H
#define WIFI_H

#include "ktypes.h"

/* ============================================================
 * 802.11 Constants
 * ============================================================ */

#define WIFI_MAX_SSID_LEN       32
#define WIFI_MAX_PASSWORD_LEN   64
#define WIFI_MAX_BSSID_LEN      6
#define WIFI_MAX_SCAN_RESULTS   32
#define WIFI_MAX_ASSOC_RETRIES  5

/* 802.11 Frame types */
#define WIFI_FC_TYPE_MGMT       0x00
#define WIFI_FC_TYPE_CTRL       0x01
#define WIFI_FC_TYPE_DATA       0x02

/* Management subtypes */
#define WIFI_STYPE_BEACON       0x80
#define WIFI_STYPE_PROBE_REQ    0x40
#define WIFI_STYPE_PROBE_RESP   0x50
#define WIFI_STYPE_AUTH         0xb0
#define WIFI_STYPE_ASSOC_REQ    0x00
#define WIFI_STYPE_ASSOC_RESP   0x10
#define WIFI_STYPE_DEAUTH       0xc0
#define WIFI_STYPE_DISASSOC     0xa0

/* Data subtypes */
#define WIFI_STYPE_DATA         0x08
#define WIFI_STYPE_QOS_DATA     0x88

/* Authentication algorithms */
#define WIFI_AUTH_OPEN          0
#define WIFI_AUTH_SHARED_KEY    1
#define WIFI_AUTH_FAST_BSS      2

/* WiFi states */
#define WIFI_STATE_DISCONNECTED  0
#define WIFI_STATE_SCANNING      1
#define WIFI_STATE_AUTHENTICATING 2
#define WIFI_STATE_ASSOCIATING   3
#define WIFI_STATE_CONNECTED     4

/* Security modes */
#define WIFI_SEC_NONE           0
#define WIFI_SEC_WEP            1
#define WIFI_SEC_WPA            2
#define WIFI_SEC_WPA2           3
#define WIFI_SEC_WPA_WPA2       4

/* WiFi flags */
#define WIFI_F_PRESENT          0x01
#define WIFI_F_INITIALIZED      0x02
#define WIFI_F_ASSOCIATED       0x04
#define WIFI_F_LINK_UP          0x08

/* ============================================================
 * 802.11 Frame Structures
 * ============================================================ */

/* 802.11 Frame Control */
struct wifi_frame_ctrl {
    uint16_t protocol:2;
    uint16_t type:2;
    uint16_t subtype:4;
    uint16_t to_ds:1;
    uint16_t from_ds:1;
    uint16_t more_frag:1;
    uint16_t retry:1;
    uint16_t pwr_mgmt:1;
    uint16_t more_data:1;
    uint16_t protected:1;
    uint16_t order:1;
} __attribute__((packed));

/* 802.11 Management Frame Header */
struct wifi_mgmt_hdr {
    struct wifi_frame_ctrl fc;
    uint16_t duration;
    uint8_t da[6];
    uint8_t sa[6];
    uint8_t bssid[6];
    uint16_t seq_ctrl;
} __attribute__((packed));

/* 802.11 Data Frame Header */
struct wifi_data_hdr {
    struct wifi_frame_ctrl fc;
    uint16_t duration;
    uint8_t addr1[6];  /* RA */
    uint8_t addr2[6];  /* TA */
    uint8_t addr3[6];  /* DA/SA */
    uint16_t seq_ctrl;
    uint8_t addr4[6];  /* Optional */
} __attribute__((packed));

/* Beacon Frame Body */
struct wifi_beacon {
    uint64_t timestamp;
    uint16_t beacon_interval;
    uint16_t capab_info;
} __attribute__((packed));

/* IE (Information Element) */
struct wifi_ie {
    uint8_t id;
    uint8_t len;
    uint8_t data[];
} __attribute__((packed));

/* IE IDs */
#define WIFI_IE_SSID            0
#define WIFI_IE_RATES           1
#define WIFI_IE_DS_PARAM        3
#define WIFI_IE_TIM             5
#define WIFI_IE_COUNTRY         7
#define WIFI_IE_RSN             48  /* WPA2 */
#define WIFI_IE_VENDOR          221 /* WPA */

/* ============================================================
 * WiFi Configuration
 * ============================================================ */

struct wifi_config {
    char ssid[WIFI_MAX_SSID_LEN + 1];
    char password[WIFI_MAX_PASSWORD_LEN + 1];
    uint8_t bssid[WIFI_MAX_BSSID_LEN];
    int security;         /* WIFI_SEC_* */
    int channel;          /* 0 = auto */
    int hidden;           /* Hidden network */
};

/* ============================================================
 * WiFi Scan Result
 * ============================================================ */

struct wifi_scan_result {
    char ssid[WIFI_MAX_SSID_LEN + 1];
    uint8_t bssid[WIFI_MAX_BSSID_LEN];
    int channel;
    int rssi;
    int security;
    int wpa_version;
    uint16_t capab_info;
    uint8_t rates[8];
    int rate_count;
};

/* ============================================================
 * WiFi Device (Driver-specific)
 * ============================================================ */

struct wifi_device;

struct wifi_ops {
    int (*init)(struct wifi_device *dev);
    int (*deinit)(struct wifi_device *dev);
    int (*start)(struct wifi_device *dev);
    int (*stop)(struct wifi_device *dev);
    int (*scan)(struct wifi_device *dev);
    int (*connect)(struct wifi_device *dev, struct wifi_config *config);
    int (*disconnect)(struct wifi_device *dev);
    int (*send)(struct wifi_device *dev, const void *data, size_t len);
    int (*recv)(struct wifi_device *dev, void *data, size_t len);
    int (*set_channel)(struct wifi_device *dev, int channel);
    int (*get_rssi)(struct wifi_device *dev);
};

struct wifi_device {
    char name[16];
    int id;
    uint8_t mac[6];
    int state;
    int flags;

    struct wifi_config config;
    struct wifi_scan_result scan_results[WIFI_MAX_SCAN_RESULTS];
    int scan_count;

    /* Driver-specific */
    struct wifi_ops *ops;
    void *priv;

    /* Statistics */
    uint64_t tx_packets;
    uint64_t rx_packets;
    uint64_t tx_bytes;
    uint64_t rx_bytes;
    uint64_t tx_errors;
    uint64_t rx_errors;
};

/* ============================================================
 * WPA/WPA2 Structures
 * ============================================================ */

/* EAPOL Frame */
struct eapol_frame {
    uint8_t version;
    uint8_t type;
    uint16_t len;
    uint8_t payload[];
} __attribute__((packed));

#define EAPOL_VERSION       1
#define EAPOL_KEY           3

/* WPA Key Info */
struct wpa_key_info {
    uint16_t key_type:1;
    uint16_t key_index:2;
    uint16_t install:1;
    uint16_t key_ack:1;
    uint16_t key_mic:1;
    uint16_t secure:1;
    uint16_t error:1;
    uint16_t request:1;
    uint16_t encrypted:1;
    uint16_t rsvd:7;
} __attribute__((packed));

/* ============================================================
 * WiFi Subsystem API
 * ============================================================ */

/* Initialize WiFi subsystem */
int wifi_init(void);

/* Register a WiFi device */
int wifi_register_device(struct wifi_device *dev);

/* Unregister a WiFi device */
int wifi_unregister_device(struct wifi_device *dev);

/* Get WiFi device by index */
struct wifi_device *wifi_get_device(int index);

/* Get WiFi device by name */
struct wifi_device *wifi_get_device_by_name(const char *name);

/* Scan for networks */
int wifi_scan(struct wifi_device *dev);

/* Connect to a network */
int wifi_connect(struct wifi_device *dev, struct wifi_config *config);

/* Disconnect from network */
int wifi_disconnect(struct wifi_device *dev);

/* Send data (802.11 frame) */
int wifi_send(struct wifi_device *dev, const void *data, size_t len);

/* Receive data */
int wifi_recv(struct wifi_device *dev, void *data, size_t len);

/* Get connection status */
int wifi_get_state(struct wifi_device *dev);

/* Print scan results */
void wifi_print_scan_results(struct wifi_device *dev);

/* WiFi tick (call from timer/main loop) */
void wifi_tick(void);

/* ============================================================
 * WiFi Network Integration (with net.c)
 * ============================================================ */

/* Bridge WiFi to network stack */
int wifi_net_send_packet(struct wifi_device *dev, const void *data, size_t len);
void wifi_net_recv_packet(struct wifi_device *dev, const void *data, size_t len);

/* Auto-connect from config file */
int wifi_autoconnect(void);

/* ============================================================
 * Utility Functions
 * ============================================================ */

/* Security mode to string */
const char *wifi_security_str(int security);

/* WiFi state to string */
const char *wifi_state_str(int state);

/* Calculate WPA MIC */
void wpa_calc_mic(const uint8_t *key, const void *data, size_t len, uint8_t *mic);

/* Derive PTK (Pairwise Transient Key) */
void wpa_derive_ptk(const uint8_t *pmk, const uint8_t *anonce,
                    const uint8_t *snonce, const uint8_t *aa,
                    const uint8_t *spa, uint8_t *ptk);

/* Derive PMK from passphrase */
void wpa_derive_pmk(const char *passphrase, const char *ssid, uint8_t *pmk);

/* Encrypt/decrypt WPA frame */
int wpa_encrypt_frame(const uint8_t *ptk, const void *plain, size_t len, void *encrypted);
int wpa_decrypt_frame(const uint8_t *ptk, const void *encrypted, size_t len, void *plain);

#endif /* WIFI_H */
