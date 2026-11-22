/* deshab_ap.c — 最小 nl80211 AP 工具（开放网络，零 OpenSSL 依赖）
 *
 * 背景：Deshab guest 里 hostapd/curl 等链接 libcrypto.so.3(OpenSSL) 的程序在
 * 本 VMM(L2) 下无法运行（SIGSEGV/挂起）。而造一个桌面 WiFi 卡片能扫到的 AP
 * 需要 hostapd。开放网络（无 WPA）本不需要任何加密库，故此处用一个纯 libc +
 * 手写 netlink 的最小工具直接下发 NL80211_CMD_START_AP，绕开整套 libcrypto。
 *
 * 静态编译（-static），完全不依赖 guest 的 ld-linux / libcrypto。
 *
 * 用法：deshab_ap <ifname> <ssid> [freq_mhz]
 *   例：deshab_ap wlan1 Deshab-Net 2412
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <linux/netlink.h>
#include <linux/genetlink.h>
#include <linux/nl80211.h>

#ifndef NLA_F_NESTED
#define NLA_F_NESTED (1 << 15)
#endif
#ifndef NLA_OK
#define NLA_OK(nla, len) ((len) >= (int)sizeof(struct nlattr) && \
    (nla)->nla_len >= sizeof(struct nlattr) && (nla)->nla_len <= (len))
#define NLA_NEXT(nla, attrlen) ((attrlen) -= NLA_ALIGN((nla)->nla_len), \
    (struct nlattr *)(((char *)(nla)) + NLA_ALIGN((nla)->nla_len)))
#endif

#define NLMSG_TAIL(nmsg) \
    ((struct nlattr *)(((char *)(nmsg)) + NLMSG_ALIGN((nmsg)->nlmsg_len)))

static int nlfd = -1;
static uint32_t nlseq = 1;
static int nl80211_id = -1;

/* ---- 属性构造 ---- */
static void nla_put(struct nlmsghdr *nlh, int type, const void *data, int len) {
    struct nlattr *nla = NLMSG_TAIL(nlh);
    nla->nla_type = type;
    nla->nla_len = (uint16_t)(NLA_HDRLEN + len);
    if (data && len) memcpy((char *)nla + NLA_HDRLEN, data, len);
    nlh->nlmsg_len = (uint32_t)(NLMSG_ALIGN(nlh->nlmsg_len) +
                                NLA_ALIGN(nla->nla_len));
}
static struct nlattr *nla_nest_start(struct nlmsghdr *nlh, int type) {
    struct nlattr *nla = NLMSG_TAIL(nlh);
    nla->nla_type = (uint16_t)(type | NLA_F_NESTED);
    nla->nla_len = NLA_HDRLEN;
    nlh->nlmsg_len = (uint32_t)(NLMSG_ALIGN(nlh->nlmsg_len) +
                                NLA_ALIGN(NLA_HDRLEN));
    return nla;
}
static void nla_nest_end(struct nlmsghdr *nlh, struct nlattr *nla) {
    nla->nla_len = (uint16_t)((char *)NLMSG_TAIL(nlh) - (char *)nla);
}
static void nla_put_u16(struct nlmsghdr *nlh, int t, uint16_t v) { nla_put(nlh, t, &v, 2); }
static void nla_put_u32(struct nlmsghdr *nlh, int t, uint32_t v) { nla_put(nlh, t, &v, 4); }
static void nla_put_u8(struct nlmsghdr *nlh, int t, uint8_t v) { nla_put(nlh, t, &v, 1); }

/* ---- netlink 收发 ---- */
static int nl_open(void) {
    nlfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_GENERIC);
    if (nlfd < 0) { perror("socket"); return -1; }
    struct sockaddr_nl sa = { .nl_family = AF_NETLINK };
    if (bind(nlfd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return -1; }
    return 0;
}

static int nl_send(struct nlmsghdr *nlh) {
    struct sockaddr_nl dst = { .nl_family = AF_NETLINK };
    nlh->nlmsg_seq = nlseq++;
    nlh->nlmsg_pid = 0;
    return (int)sendto(nlfd, nlh, nlh->nlmsg_len, 0,
                       (struct sockaddr *)&dst, sizeof(dst));
}

/* 等待匹配 seq 的响应；返回 NLMSG_ERROR->error（0=OK），解析 genl family id。*/
static int nl_recv(int want_seq, int *out_family) {
    char buf[8192];
    for (;;) {
        int n = (int)recv(nlfd, buf, sizeof(buf), 0);
        if (n < 0) { perror("recv"); return -1; }
        for (struct nlmsghdr *h = (struct nlmsghdr *)buf; NLMSG_OK(h, n);
             h = NLMSG_NEXT(h, n)) {
            if (h->nlmsg_seq != (uint32_t)want_seq) continue;
            if (h->nlmsg_type == NLMSG_ERROR) {
                struct nlmsgerr *e = (struct nlmsgerr *)NLMSG_DATA(h);
                return e->error;   /* 0 = success */
            }
            if (out_family && h->nlmsg_type == GENL_ID_CTRL) {
                struct genlmsghdr *g = (struct genlmsghdr *)NLMSG_DATA(h);
                int alen = (int)(h->nlmsg_len - NLMSG_LENGTH(sizeof(*g)));
                struct nlattr *a = (struct nlattr *)((char *)g + GENL_HDRLEN);
                for (; NLA_OK(a, alen); a = NLA_NEXT(a, alen)) {
                    if (a->nla_type == CTRL_ATTR_FAMILY_ID) {
                        *out_family = *(uint16_t *)((char *)a + NLA_HDRLEN);
                        return 0;
                    }
                }
            }
            return -2;
        }
    }
}

/* ---- nl80211 命令 ---- */
static int nl80211_send(int cmd, void (*fill)(struct nlmsghdr *, void *), void *arg) {
    char buf[4096];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    nlh->nlmsg_type = (uint16_t)nl80211_id;
    nlh->nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    ((struct genlmsghdr *)NLMSG_DATA(nlh))->cmd = (uint8_t)cmd;
    ((struct genlmsghdr *)NLMSG_DATA(nlh))->version = 1;
    if (fill) fill(nlh, arg);
    int seq = nlseq;
    if (nl_send(nlh) < 0) { perror("sendto"); return -1; }
    return nl_recv(seq, NULL);
}

static int nl80211_resolve(void) {
    char buf[512];
    memset(buf, 0, sizeof(buf));
    struct nlmsghdr *nlh = (struct nlmsghdr *)buf;
    nlh->nlmsg_len = NLMSG_LENGTH(GENL_HDRLEN);
    nlh->nlmsg_type = GENL_ID_CTRL;
    nlh->nlmsg_flags = NLM_F_REQUEST;
    struct genlmsghdr *g = (struct genlmsghdr *)NLMSG_DATA(nlh);
    g->cmd = CTRL_CMD_GETFAMILY;
    g->version = 1;
    const char *name = "nl80211";
    nla_put(nlh, CTRL_ATTR_FAMILY_NAME, name, (int)strlen(name) + 1);
    int seq = nlseq;
    if (nl_send(nlh) < 0) return -1;
    int fam = -1;
    if (nl_recv(seq, &fam) != 0 || fam < 0) return -1;
    nl80211_id = fam;
    return 0;
}

/* ---- 具体命令 ---- */
static void fill_set_iface(struct nlmsghdr *nlh, void *arg) {
    int ifindex = *(int *)arg;
    nla_put_u32(nlh, NL80211_ATTR_IFINDEX, (uint32_t)ifindex);
    nla_put_u32(nlh, NL80211_ATTR_IFTYPE, NL80211_IFTYPE_AP);
}

struct ap_arg { int ifindex; const char *ssid; int freq; uint8_t mac[6]; };

/* 手工拼 802.11 beacon 帧头（MAC 头 + 定长字段 + IEs，不含 TIM——内核会插 TIM）。*/
static int build_beacon_head(const struct ap_arg *a, uint8_t *b, int cap) {
    int n = 0;
    #define P8(v)  do { b[n++] = (uint8_t)(v); } while (0)
    #define P16(v) do { b[n++] = (uint8_t)((v) & 0xff); b[n++] = (uint8_t)((v) >> 8); } while (0)
    /* --- MAC header (24) --- */
    P16(0x0080);                       /* frame control: beacon */
    P16(0x0000);                       /* duration */
    for (int i = 0; i < 6; i++) P8(0xff);        /* DA = broadcast */
    for (int i = 0; i < 6; i++) P8(a->mac[i]);   /* SA = BSSID */
    for (int i = 0; i < 6; i++) P8(a->mac[i]);   /* BSSID */
    P16(0x0000);                       /* seq ctrl */
    /* --- fixed params (12) --- */
    for (int i = 0; i < 8; i++) P8(0); /* timestamp */
    P16(100);                          /* beacon interval */
    P16(0x0001);                       /* capability: ESS, no privacy */
    /* --- SSID IE --- */
    int sl = (int)strlen(a->ssid);
    P8(0); P8(sl);
    for (int i = 0; i < sl; i++) P8((uint8_t)a->ssid[i]);
    /* --- Supported Rates IE (1,2,5.5,11,6,9,12,18 Mbps) --- */
    P8(1); P8(8);
    P8(0x82); P8(0x84); P8(0x8b); P8(0x96); P8(0x0c); P8(0x12); P8(0x18); P8(0x24);
    /* --- DS Params IE (channel) --- */
    int ch = (a->freq - 2407) / 5;
    P8(3); P8(1); P8(ch);
    (void)cap;
    return n;
}

static void fill_start_ap(struct nlmsghdr *nlh, void *arg) {
    struct ap_arg *a = (struct ap_arg *)arg;
    uint8_t bcn[256];
    int blen = build_beacon_head(a, bcn, sizeof(bcn));
    nla_put_u32(nlh, NL80211_ATTR_IFINDEX, (uint32_t)a->ifindex);
    nla_put_u32(nlh, NL80211_ATTR_BEACON_INTERVAL, 100);
    nla_put_u32(nlh, NL80211_ATTR_DTIM_PERIOD, 2);
    nla_put(nlh, NL80211_ATTR_SSID, a->ssid, (int)strlen(a->ssid));
    nla_put(nlh, NL80211_ATTR_BEACON_HEAD, bcn, blen);   /* 内核补 TIM + 帧检查码 */
    nla_put(nlh, NL80211_ATTR_WIPHY_FREQ, &a->freq, 4);
    nla_put_u32(nlh, NL80211_ATTR_AUTH_TYPE, 0);          /* open */
    /* 单频 20MHz 即默认无 HT，无需 CHANNEL_TYPE（该常量已在新内核头文件移除） */
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <ifname> <ssid> [freq]\n", argv[0]);
        return 2;
    }
    const char *ifname = argv[1];
    const char *ssid = argv[2];
    int freq = (argc >= 4) ? atoi(argv[3]) : 2412;

    if (nl_open() != 0) return 1;
    if (nl80211_resolve() != 0) { fprintf(stderr, "nl80211 family not found\n"); return 1; }

    int ifindex = if_nametoindex(ifname);
    if (ifindex <= 0) { fprintf(stderr, "no such iface: %s\n", ifname); return 1; }

    /* 取本接口 MAC 作 BSSID */
    struct ap_arg a; memset(&a, 0, sizeof(a));
    a.ifindex = ifindex; a.ssid = ssid; a.freq = freq;
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    struct ifreq ifr; memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) == 0)
        memcpy(a.mac, ifr.ifr_hwaddr.sa_data, 6);
    close(s);

    /* 1) 设为 AP 模式 */
    int rc = nl80211_send(NL80211_CMD_SET_INTERFACE, fill_set_iface, &ifindex);
    fprintf(stderr, "[ap] set iface AP rc=%d\n", rc);
    if (rc != 0) return 1;

    /* 2) 起 AP */
    rc = nl80211_send(NL80211_CMD_START_AP, fill_start_ap, &a);
    fprintf(stderr, "[ap] start_ap ssid=%s freq=%d rc=%d\n", ssid, freq, rc);
    if (rc != 0) return 1;

    fprintf(stderr, "[ap] AP up on %s (%s)\n", ifname, ssid);
    /* 保持运行，维持 AP（内核侧 beacon 由 mac80211 接管，进程退出一般也行，
     * 但保持存活最稳）。 */
    for (;;) pause();
    return 0;
}
