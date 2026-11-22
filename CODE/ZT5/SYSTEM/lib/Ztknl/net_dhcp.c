/*
 * Network Auto-Connect and DHCP Client Manager
 *
 * Handles:
 * 1. Reading network configuration files (e1w.cfg format)
 * 2. Starting DHCP client automatically
 * 3. Managing network interface state
 */

#include "net.h"
#include "dhcp.h"
#include "console.h"
#include "string.h"
#include "heap.h"
#include "vfs.h"
#include "cpu.h"

/* Global DHCP client instance */
static struct dhcp_client g_dhcp_client;
static int g_dhcp_initialized = 0;

/* Network configuration */
struct net_config {
    char ssid[64];        /* WiFi network name (for future use) */
    char password[64];    /* WiFi password (for future use) */
    int use_dhcp;         /* Use DHCP (1) or static IP (0) */
    uint32_t static_ip;
    uint32_t netmask;
    uint32_t gateway;
    uint32_t dns;
    int configured;
};

struct net_config g_net_config;

/* ============================================================
 * Network Config Parser (e1w.cfg format)
 *
 * Format:
 *   name = "network_ssid"
 *   password = "password"
 * ============================================================ */

static void trim_quotes(char *str) {
    /* Remove leading/trailing quotes and whitespace */
    char *start = str;
    while (*start == ' ' || *start == '\t') start++;
    if (*start == '"') start++;

    char *end = start + strlen(start) - 1;
    while (end > start && (*end == ' ' || *end == '\t' || *end == '\n' || *end == '\r')) end--;
    if (*end == '"') *end = '\0';

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

int net_config_parse(const char *path) {
    printk("net: Parsing network config: %s", path);

    /* Open file via VFS */
    struct vfs_file *file = vfs_open(path, 0);
    if (file == NULL) {
        printk("net: Failed to open %s", path);
        return -1;
    }

    /* Read file content */
    char buf[512];
    ssize_t n = vfs_read(file, buf, sizeof(buf) - 1);
    if (n <= 0) {
        vfs_close(file);
        printk("net: Empty or unreadable config file");
        return -1;
    }
    buf[n] = '\0';
    vfs_close(file);

    /* Parse line by line */
    char *line = buf;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) {
            *next = '\0';
            next++;
        }

        /* Skip empty lines and comments */
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') {
            line = next;
            continue;
        }

        /* Parse key = value */
        char *eq = strchr(p, '=');
        if (eq) {
            *eq = '\0';
            char *key = p;
            char *value = eq + 1;

            /* Trim key and value */
            char *end = key + strlen(key) - 1;
            while (end > key && (*end == ' ' || *end == '\t')) *end-- = '\0';
            trim_quotes(value);

            if (strcmp(key, "name") == 0) {
                strncpy(g_net_config.ssid, value, sizeof(g_net_config.ssid) - 1);
                printk("net: Network name: %s", g_net_config.ssid);
            } else if (strcmp(key, "password") == 0) {
                strncpy(g_net_config.password, value, sizeof(g_net_config.password) - 1);
                printk("net: Password configured (length=%d)", (int)strlen(g_net_config.password));
            }

            g_net_config.configured = 1;
        }

        line = next;
    }

    printk("net: Network config parsed successfully");
    return 0;
}

/* ============================================================
 * DHCP Callback
 * ============================================================ */

static void dhcp_on_bound(struct dhcp_client *client) {
    printk("=== DHCP BOUND ===");
    printk("  IP Address : %u.%u.%u.%u",
           (client->config.ip_address >> 24) & 0xFF,
           (client->config.ip_address >> 16) & 0xFF,
           (client->config.ip_address >> 8) & 0xFF,
           client->config.ip_address & 0xFF);
    printk("  Subnet Mask: %u.%u.%u.%u",
           (client->config.subnet_mask >> 24) & 0xFF,
           (client->config.subnet_mask >> 16) & 0xFF,
           (client->config.subnet_mask >> 8) & 0xFF,
           client->config.subnet_mask & 0xFF);
    printk("  Gateway    : %u.%u.%u.%u",
           (client->config.router >> 24) & 0xFF,
           (client->config.router >> 16) & 0xFF,
           (client->config.router >> 8) & 0xFF,
           client->config.router & 0xFF);
    printk("  DNS Server : %u.%u.%u.%u",
           (client->config.dns_servers[0] >> 24) & 0xFF,
           (client->config.dns_servers[0] >> 16) & 0xFF,
           (client->config.dns_servers[0] >> 8) & 0xFF,
           client->config.dns_servers[0] & 0xFF);
    printk("  Lease Time : %u seconds", client->config.lease_time);

    /* Update NIC configuration */
    if (client->nic) {
        client->nic->ip_addr = client->config.ip_address;
        client->nic->netmask = client->config.subnet_mask;
        client->nic->gateway = client->config.router;
        client->nic->state = NIC_STATE_RUNNING;
    }

    g_dhcp_initialized = 1;
}

static void dhcp_on_timeout(struct dhcp_client *client) {
    printk("net: DHCP timeout (attempt %d)", client->retries);
    (void)client;
}

/* ============================================================
 * DHCP Input Bridge
 *
 * Called from net_handle_udp() when a DHCP response is received.
 * Bridges the UDP layer to the DHCP client state machine.
 * ============================================================ */

void net_dhcp_input(struct nic_device *nic, const void *data, size_t len) {
    if (g_dhcp_client.nic) {
        /* Create a temporary net_buf from the raw data */
        struct net_buf *buf = net_buf_alloc();
        if (buf) {
            memcpy(buf->data, data, len < ETH_FRAME_LEN ? len : ETH_FRAME_LEN);
            buf->len = len < ETH_FRAME_LEN ? len : ETH_FRAME_LEN;
            dhcp_input(&g_dhcp_client, buf);
            net_buf_free(buf);
        }
    }
}

/* ============================================================
 * DHCP Start
 * ============================================================ */

int net_dhcp_start(struct nic_device *nic) {
    if (!nic) return -1;

    printk("net: Starting DHCP on %s (MAC: %02x:%02x:%02x:%02x:%02x:%02x)",
           nic->name,
           nic->mac.addr[0], nic->mac.addr[1], nic->mac.addr[2],
           nic->mac.addr[3], nic->mac.addr[4], nic->mac.addr[5]);

    /* Initialize DHCP client */
    if (dhcp_client_init(&g_dhcp_client, nic) < 0) {
        printk("net: Failed to init DHCP client");
        return -1;
    }

    /* Set callbacks */
    g_dhcp_client.on_bound = dhcp_on_bound;
    g_dhcp_client.on_timeout = dhcp_on_timeout;

    /* Start DHCP */
    if (dhcp_start(&g_dhcp_client) < 0) {
        printk("net: Failed to start DHCP");
        return -1;
    }

    printk("net: DHCP Discover sent, waiting for offer...");
    return 0;
}

/* ============================================================
 * DHCP Tick (call from main loop or timer)
 * ============================================================ */

void net_dhcp_tick(void) {
    if (g_dhcp_client.nic && !g_dhcp_initialized) {
        dhcp_tick(&g_dhcp_client);
    }
}

/* ============================================================
 * Network Auto-Connect
 *
 * Tries to connect network:
 * 1. Parse config file (if exists)
 * 2. Start DHCP
 * ============================================================ */

int net_autoconnect(void) {
    printk("=== Network Auto-Connect ===");

    /* Reset config */
    memset(&g_net_config, 0, sizeof(g_net_config));

    /* Try to parse network config */
    net_config_parse("/system/internet/e1w.cfg");

    /* Find the first usable NIC (skip loopback) */
    struct nic_device *nic = net_get_first_nic();

    if (!nic) {
        printk("net: No usable network interface found");
        return -1;
    }

    printk("net: Using interface: %s", nic->name);

    /* For now, always use DHCP (WiFi config is stored but not used yet) */
    /* WiFi support requires additional wireless drivers */

    /* Start DHCP */
    return net_dhcp_start(nic);
}

/* ============================================================
 * Status Functions
 * ============================================================ */

int net_is_connected(void) {
    return g_dhcp_initialized;
}

uint32_t net_get_ip(void) {
    return g_dhcp_client.config.ip_address;
}

uint32_t net_get_gateway(void) {
    return g_dhcp_client.config.router;
}

uint32_t net_get_dns(void) {
    return g_dhcp_client.config.dns_servers[0];
}
