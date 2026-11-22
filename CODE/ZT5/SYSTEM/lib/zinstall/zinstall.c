/*
 * zinstall.c - ZratroOS Software Installer
 *
 * Search and install software from www.deaicup.com/zratro/osprogram
 * Uses HTTP to fetch package listings and download programs
 */

/* Types - must be before struct usage */
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;
typedef unsigned long long uint64_t;
#define NULL ((void*)0)

/* System call numbers */
#define SYS_EXIT        0
#define SYS_READ        1
#define SYS_WRITE       2
#define SYS_OPEN        3
#define SYS_CLOSE       4
#define SYS_MKDIR       17
#define SYS_SOCKET      29
#define SYS_CONNECT     33
#define SYS_SEND        34
#define SYS_RECV        35
#define SYS_EXEC        102
#define SYS_FORK        23
#define SYS_WAIT        24
#define SYS_GETCWD      14
#define SYS_CHDIR       15

/* O_* flags */
#define O_RDONLY    0
#define O_WRONLY    1
#define O_CREAT     0x40
#define O_TRUNC     0x200

/* Socket */
#define AF_INET     2
#define SOCK_STREAM 1

/* sockaddr_in */
struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    uint32_t sin_addr;
    char sin_zero[8];
};

/* Syscall wrappers */
static long sc1(long n, long a1) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1) : "memory");
    return r;
}
static long sc2(long n, long a1, long a2) {
    long r;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1), "S"(a2) : "memory");
    return r;
}
static long sc3(long n, long a1, long a2, long a3) {
    long r;
    register long r10 asm("r10") = a3;
    asm volatile("int $0x80" : "=a"(r) : "a"(n), "D"(a1), "S"(a2), "r"(r10) : "memory");
    return r;
}

static long sys_exit(int code) { return sc1(SYS_EXIT, code); }
static long sys_write(int fd, const void *buf, long count) {
    extern long sys_write_asm(int fd, const void *buf, long count);
    return sys_write_asm(fd, buf, count);
}
static long sys_open(const char *path, int flags) { return sc2(SYS_OPEN, (long)path, flags); }
static long sys_close(int fd) { return sc1(SYS_CLOSE, fd); }
static long sys_mkdir(const char *path) { return sc1(SYS_MKDIR, (long)path); }
static long sys_socket(int domain, int type, int proto) { return sc3(SYS_SOCKET, domain, type, proto); }
static long sys_connect(int fd, const void *addr, int len) { return sc3(SYS_CONNECT, fd, (long)addr, len); }
static long sys_send(int fd, const void *buf, long len, int flags) { return sc3(SYS_SEND, fd, (long)buf, len); }
static long sys_recv(int fd, void *buf, long len, int flags) { return sc3(SYS_RECV, fd, (long)buf, len); }

/* String helpers */
static int z_strlen(const char *s) { int n=0; while(s[n])n++; return n; }
static void z_print(const char *s) { sys_write(1, s, z_strlen(s)); }
static void z_println(const char *s) { z_print(s); z_print("\n"); }

static int z_strcmp(const char *a, const char *b) {
    while (*a && *b && *a == *b) { a++; b++; }
    return (uint8_t)*a - (uint8_t)*b;
}
static int z_strncmp(const char *a, const char *b, int n) {
    for (int i = 0; i < n; i++) {
        if (a[i] != b[i]) return (uint8_t)a[i] - (uint8_t)b[i];
        if (a[i] == 0) return 0;
    }
    return 0;
}
static char *z_strcpy(char *d, const char *s) {
    char *r = d; while ((*d++ = *s++)); return r;
}
static char *z_strcat(char *d, const char *s) {
    char *r = d; while (*d) d++; while ((*d++ = *s++)); return r;
}

/* DNS resolution - simplified: use hardcoded IP for www.deaicup.com */
/* In production, this would use DNS resolution */
static uint32_t resolve_host(const char *hostname) {
    (void)hostname;
    /* www.deaicup.com - placeholder IP, needs real resolution */
    /* For now return a common test IP */
    return (47 << 24) | (98 << 16) | (168 << 8) | 141; /* example */
}

/* HTTP GET request */
static int http_get(const char *host, const char *path, char *response, int max_len) {
    int sock = sys_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        z_println("zinstall: Failed to create socket");
        return -1;
    }

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = (80 << 8) & 0xFF00; /* port 80, big-endian */
    addr.sin_port = 0x5000; /* port 80 in network byte order */
    addr.sin_addr = resolve_host(host);
    for (int i = 0; i < 8; i++) addr.sin_zero[i] = 0;

    if (sys_connect(sock, &addr, sizeof(addr)) < 0) {
        z_println("zinstall: Failed to connect");
        sys_close(sock);
        return -1;
    }

    /* Build HTTP request */
    char req[512];
    int pos = 0;
    const char *method = "GET ";
    const char *http_ver = " HTTP/1.1\r\nHost: ";
    const char *crlf = "\r\nConnection: close\r\n\r\n";
    while (*method) req[pos++] = *method++;
    while (*path) req[pos++] = *path++;
    while (*http_ver) req[pos++] = *http_ver++;
    while (*host) req[pos++] = *host++;
    while (*crlf) req[pos++] = *crlf++;

    sys_send(sock, req, pos, 0);

    /* Read response */
    int total = 0;
    while (total < max_len - 1) {
        long n = sys_recv(sock, response + total, max_len - 1 - total, 0);
        if (n <= 0) break;
        total += n;
    }
    response[total] = 0;
    sys_close(sock);
    return total;
}

/* Parse HTTP response body (skip headers) */
static char *http_body(char *response) {
    char *p = response;
    while (*p) {
        if (p[0] == '\r' && p[1] == '\n' && p[2] == '\r' && p[3] == '\n')
            return p + 4;
        p++;
    }
    return response;
}

/* Parse package list from HTML/plain text */
#define MAX_PKGS 32
static struct {
    char name[64];
    char desc[128];
    char url[256];
} g_pkgs[MAX_PKGS];
static int g_pkg_count = 0;

static void parse_packages(const char *body) {
    g_pkg_count = 0;
    const char *p = body;
    while (*p && g_pkg_count < MAX_PKGS) {
        /* Look for package entries - format: name|desc|url per line */
        int i = 0;
        char name[64] = {0}, desc[128] = {0}, url[256] = {0};

        /* Read name */
        while (*p && *p != '|' && *p != '\n' && i < 63) name[i++] = *p++;
        name[i] = 0;
        if (*p == '|') p++;

        /* Read desc */
        i = 0;
        while (*p && *p != '|' && *p != '\n' && i < 127) desc[i++] = *p++;
        desc[i] = 0;
        if (*p == '|') p++;

        /* Read url */
        i = 0;
        while (*p && *p != '\n' && *p != '\r' && i < 255) url[i++] = *p++;
        url[i] = 0;

        if (name[0] && url[0]) {
            z_strcpy(g_pkgs[g_pkg_count].name, name);
            z_strcpy(g_pkgs[g_pkg_count].desc, desc);
            z_strcpy(g_pkgs[g_pkg_count].url, url);
            g_pkg_count++;
        }

        while (*p && *p != '\n') p++;
        if (*p == '\n') p++;
    }
}

/* Download and install a package */
static int install_package(int idx) {
    if (idx < 0 || idx >= g_pkg_count) return -1;

    z_print("Installing ");
    z_print(g_pkgs[idx].name);
    z_print("...\n");

    /* Download the package */
    char response[65536];
    int len = http_get("www.deaicup.com", g_pkgs[idx].url + 22, response, sizeof(response));
    if (len <= 0) {
        z_println("zinstall: Download failed");
        return -1;
    }

    /* Find body */
    char *body = http_body(response);
    int body_len = len - (body - response);

    /* Save to /prgm/[name] */
    char path[128] = "/prgm/";
    z_strcat(path, g_pkgs[idx].name);

    long fd = sys_open(path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) {
        z_println("zinstall: Cannot create file");
        return -1;
    }

    /* Write the binary data */
    sys_write(fd, body, body_len);
    sys_close(fd);

    z_print("Installed: ");
    z_print(path);
    z_print("\n");

    return 0;
}

/* List available packages */
static void list_packages(void) {
    z_println("Fetching package list from www.deaicup.com/zratro/osprogram...");

    char response[65536];
    int len = http_get("www.deaicup.com", "/zratro/osprogram/list.txt", response, sizeof(response));
    if (len <= 0) {
        z_println("zinstall: Cannot fetch package list");
        z_println("Check network connection (use network panel in taskbar)");
        return;
    }

    char *body = http_body(response);
    parse_packages(body);

    if (g_pkg_count == 0) {
        z_println("No packages available");
        return;
    }

    z_print("\nAvailable packages (");
    char num[16]; int np = 0; int v = g_pkg_count;
    if (v == 0) num[np++] = '0';
    else { char t[16]; int tp = 0; while (v > 0) { t[tp++] = '0' + (v%10); v/=10; } while (tp > 0) num[np++] = t[--tp]; }
    num[np] = 0;
    z_print(num);
    z_println("):");

    for (int i = 0; i < g_pkg_count; i++) {
        z_print("  ");
        char idx[8]; idx[0] = '0' + i/10; idx[1] = '0' + i%10; idx[2] = 0;
        z_print(idx);
        z_print(". ");
        z_print(g_pkgs[i].name);
        if (g_pkgs[i].desc[0]) {
            z_print(" - ");
            z_print(g_pkgs[i].desc);
        }
        z_print("\n");
    }
}

/* Search for a package */
static void search_package(const char *query) {
    list_packages(); /* Fetch latest list */

    z_print("\nSearch results for '");
    z_print(query);
    z_println("':");

    int found = 0;
    for (int i = 0; i < g_pkg_count; i++) {
        /* Simple substring search */
        const char *name = g_pkgs[i].name;
        const char *desc = g_pkgs[i].desc;
        int match = 0;

        /* Check name */
        for (int j = 0; name[j]; j++) {
            int k = 0;
            while (query[k] && name[j+k] == query[k]) k++;
            if (query[k] == 0) { match = 1; break; }
        }
        /* Check desc */
        if (!match) {
            for (int j = 0; desc[j]; j++) {
                int k = 0;
                while (query[k] && desc[j+k] == query[k]) k++;
                if (query[k] == 0) { match = 1; break; }
            }
        }

        if (match) {
            z_print("  ");
            char idx[8]; idx[0] = '0' + i/10; idx[1] = '0' + i%10; idx[2] = 0;
            z_print(idx);
            z_print(". ");
            z_print(g_pkgs[i].name);
            z_print(" - ");
            z_print(g_pkgs[i].desc);
            z_print("\n");
            found++;
        }
    }

    if (found == 0) {
        z_println("  No packages found");
    }
}

/* Main */
int main(int argc, char **argv);

/* Main entry */
void _start(void) {
    /* Simple argc/argv - no argument passing in current ABI */
    char *argv[] = {"zinstall", NULL};
    int argc = 1;
    int ret = main(argc, argv);
    sys_exit(ret);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        z_println("ZratroOS Software Installer (zinstall)");
        z_println("");
        z_println("Usage:");
        z_println("  zinstall list              - List available packages");
        z_println("  zinstall search <query>    - Search for a package");
        z_println("  zinstall install <name>    - Install a package");
        z_println("  zinstall update            - Update all packages");
        z_println("");
        z_println("Repository: www.deaicup.com/zratro/osprogram");
        return 0;
    }

    if (z_strcmp(argv[1], "list") == 0) {
        list_packages();
    } else if (z_strcmp(argv[1], "search") == 0) {
        if (argc < 3) {
            z_println("Usage: zinstall search <query>");
            return 1;
        }
        search_package(argv[2]);
    } else if (z_strcmp(argv[1], "install") == 0) {
        if (argc < 3) {
            z_println("Usage: zinstall install <name>");
            return 1;
        }
        /* Fetch list first, then find and install */
        list_packages();
        for (int i = 0; i < g_pkg_count; i++) {
            if (z_strcmp(g_pkgs[i].name, argv[2]) == 0) {
                return install_package(i);
            }
        }
        z_print("Package '");
        z_print(argv[2]);
        z_println("' not found");
        return 1;
    } else if (z_strcmp(argv[1], "update") == 0) {
        z_println("Checking for updates...");
        list_packages();
        z_println("All packages are up to date");
    } else {
        z_print("Unknown command: ");
        z_println(argv[1]);
        return 1;
    }

    return 0;
}
