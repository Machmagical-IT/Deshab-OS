/*
 * ZTS Header Tool
 *
 * Injects ZTS module headers into compiled .zts binary files.
 * Creates the proper ZTS7 file format with magic number, version,
 * entry point offsets, and checksums.
 *
 * Usage:
 *   ./zts_header_tool --name <name> --priority <hex> --category <cat> --inject <file.zts>
 *   ./zts_header_tool --info <file.zts>
 *   ./zts_header_tool --verify <file.zts>
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <getopt.h>
#include <sys/stat.h>

/* ZTS Header Definition (matching zts_algorithm.h) */
#define ZTS_MAGIC         0x53545A00
#define ZTS_API_VERSION   1
#define ZTS_UUID_LEN      16
#define ZTS_MAX_NAME      64
#define ZTS_MAX_DESC      256
#define ZTS_MAX_AUTHOR    64
#define ZTS_MAX_LICENSE   32

enum zts_category {
    ZTS_CAT_FILESYSTEM = 0,
    ZTS_CAT_NETWORK,
    ZTS_CAT_CRYPTO,
    ZTS_CAT_COMPRESSION,
    ZTS_CAT_SCHEDULER,
    ZTS_CAT_MEMORY,
    ZTS_CAT_DRIVER,
    ZTS_CAT_PATCH,
    ZTS_CAT_INJECT,
    ZTS_CAT_CUSTOM,
};

struct zts_header {
    uint32_t magic;              /* 0x53545A00 */
    uint16_t version;            /* API version */
    uint16_t api_min_version;    /* Minimum required API */
    char name[ZTS_MAX_NAME];
    char description[ZTS_MAX_DESC];
    char author[ZTS_MAX_AUTHOR];
    char license[ZTS_MAX_LICENSE];
    uint8_t uuid[ZTS_UUID_LEN];
    uint32_t category;
    uint32_t major_version;
    uint32_t minor_version;
    uint32_t patch_version;
    uint32_t num_dependencies;
    uint64_t init_func_offset;    /* Offset to zts_module_init */
    uint64_t cleanup_func_offset; /* Offset to zts_module_cleanup */
    uint64_t execute_func_offset; /* Offset to zts_module_exec */
    uint64_t data_offset;
    uint64_t data_size;
    uint32_t checksum;
    uint8_t signature[256];
} __attribute__((packed));

#define HEADER_SIZE sizeof(struct zts_header)

/* Symbol table entry point names */
#define INIT_SYMBOL    "zts_module_init"
#define EXEC_SYMBOL    "zts_module_exec"
#define CLEANUP_SYMBOL "zts_module_cleanup"

/* ============================================================
 * Utility Functions
 * ============================================================ */

static void generate_uuid(uint8_t uuid[ZTS_UUID_LEN]) {
    /* Simple UUID v4 generation using /dev/urandom or rand() */
    FILE *f = fopen("/dev/urandom", "r");
    if (f) {
        fread(uuid, 1, ZTS_UUID_LEN, f);
        fclose(f);
    } else {
        /* Fallback to rand() */
        for (int i = 0; i < ZTS_UUID_LEN; i++) {
            uuid[i] = (uint8_t)(rand() % 256);
        }
    }
    /* Set version to 4 (random) */
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    /* Set variant to RFC 4122 */
    uuid[8] = (uuid[8] & 0x3F) | 0x80;
}

static uint32_t calculate_checksum(uint8_t *data, size_t size) {
    uint32_t sum = 0;
    for (size_t i = 0; i < size; i++) {
        sum += data[i];
    }
    return sum;
}

static const char* category_to_string(enum zts_category cat) {
    switch (cat) {
    case ZTS_CAT_FILESYSTEM: return "FILESYSTEM";
    case ZTS_CAT_NETWORK:    return "NETWORK";
    case ZTS_CAT_CRYPTO:     return "CRYPTO";
    case ZTS_CAT_SCHEDULER:  return "SCHEDULER";
    case ZTS_CAT_MEMORY:     return "MEMORY";
    case ZTS_CAT_DRIVER:     return "DRIVER";
    case ZTS_CAT_PATCH:      return "PATCH";
    case ZTS_CAT_INJECT:     return "INJECT";
    default:                 return "CUSTOM";
    }
}

static enum zts_category string_to_category(const char *str) {
    if (strcmp(str, "FILESYSTEM") == 0) return ZTS_CAT_FILESYSTEM;
    if (strcmp(str, "NETWORK") == 0)    return ZTS_CAT_NETWORK;
    if (strcmp(str, "CRYPTO") == 0)     return ZTS_CAT_CRYPTO;
    if (strcmp(str, "SCHEDULER") == 0)  return ZTS_CAT_SCHEDULER;
    if (strcmp(str, "MEMORY") == 0)     return ZTS_CAT_MEMORY;
    if (strcmp(str, "DRIVER") == 0)     return ZTS_CAT_DRIVER;
    if (strcmp(str, "PATCH") == 0)      return ZTS_CAT_PATCH;
    if (strcmp(str, "INJECT") == 0)     return ZTS_CAT_INJECT;
    return ZTS_CAT_CUSTOM;
}

static void print_uuid(const uint8_t uuid[ZTS_UUID_LEN]) {
    printf("%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
           uuid[0], uuid[1], uuid[2], uuid[3],
           uuid[4], uuid[5], uuid[6], uuid[7],
           uuid[8], uuid[9], uuid[10], uuid[11],
           uuid[12], uuid[13], uuid[14], uuid[15]);
}

/* ============================================================
 * ELF Symbol Extraction (Simplified)
 * ============================================================ */

/*
 * Simple ELF parser to find symbol offsets.
 * This is a minimal implementation - for production use, consider
 * using libbfd or libelf.
 */
static uint64_t find_symbol_offset(const uint8_t *elf_data, size_t elf_size, 
                                   const char *symbol_name) {
    /* Basic ELF header parsing */
    if (elf_size < 64 || elf_data[0] != 0x7f || 
        elf_data[1] != 'E' || elf_data[2] != 'L' || elf_data[3] != 'F') {
        fprintf(stderr, "Warning: Not a valid ELF file\n");
        return 0;
    }
    
    /* This is a simplified version - full implementation would:
     * 1. Parse ELF header to find section header table
     * 2. Find .symtab and .strtab sections
     * 3. Search for symbol name in symbol table
     * 4. Return symbol value (offset)
     *
     * For now, we use a heuristic: scan for the symbol name string
     * and assume the function follows at a predictable offset.
     */
    
    /* Search for symbol name in binary */
    size_t name_len = strlen(symbol_name);
    for (size_t i = 0; i < elf_size - name_len; i++) {
        if (memcmp(elf_data + i, symbol_name, name_len) == 0) {
            /* Found symbol name string - in a real implementation,
             * we would parse the symbol table to get the actual offset */
            fprintf(stderr, "Info: Found symbol name '%s' at offset 0x%lx\n", 
                    symbol_name, (unsigned long)i);
            /* For now, return 0 - caller should use default offsets */
            return 0;
        }
    }
    
    return 0;
}

/* ============================================================
 * Header Injection
 * ============================================================ */

static int inject_header(const char *filename, const char *name, 
                        uint32_t priority, enum zts_category category) {
    /* Read the input file */
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("Failed to open input file");
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    uint8_t *data = malloc(file_size);
    if (!data) {
        fclose(f);
        fprintf(stderr, "Out of memory\n");
        return -1;
    }
    
    if (fread(data, 1, file_size, f) != file_size) {
        fclose(f);
        free(data);
        fprintf(stderr, "Failed to read file\n");
        return -1;
    }
    fclose(f);
    
    /* Prepare header */
    struct zts_header hdr;
    memset(&hdr, 0, sizeof(hdr));
    
    hdr.magic = ZTS_MAGIC;
    hdr.version = ZTS_API_VERSION;
    hdr.api_min_version = 1;
    
    strncpy(hdr.name, name, ZTS_MAX_NAME - 1);
    snprintf(hdr.description, ZTS_MAX_DESC, "ZTS module: %s", name);
    strncpy(hdr.author, "ZTS Build Tool", ZTS_MAX_AUTHOR - 1);
    strncpy(hdr.license, "MIT", ZTS_MAX_LICENSE - 1);
    
    generate_uuid(hdr.uuid);
    
    hdr.category = category;
    hdr.major_version = 1;
    hdr.minor_version = 0;
    hdr.patch_version = 0;
    hdr.num_dependencies = 0;
    
    /* Find entry point offsets */
    hdr.init_func_offset = find_symbol_offset(data, file_size, INIT_SYMBOL);
    hdr.cleanup_func_offset = find_symbol_offset(data, file_size, CLEANUP_SYMBOL);
    hdr.execute_func_offset = find_symbol_offset(data, file_size, EXEC_SYMBOL);
    
    /* If symbols not found, use defaults (assuming order in compiled output) */
    if (hdr.init_func_offset == 0) hdr.init_func_offset = HEADER_SIZE;
    if (hdr.execute_func_offset == 0) hdr.execute_func_offset = HEADER_SIZE + 0x100;
    if (hdr.cleanup_func_offset == 0) hdr.cleanup_func_offset = HEADER_SIZE + 0x200;
    
    hdr.data_offset = HEADER_SIZE + file_size;
    hdr.data_size = 0;
    
    /* Calculate checksum */
    hdr.checksum = 0; /* Clear checksum field first */
    hdr.checksum = calculate_checksum((uint8_t*)&hdr, sizeof(hdr));
    
    /* Write output file: header + original data */
    char output_filename[512];
    snprintf(output_filename, sizeof(output_filename), "%s.hdr", filename);
    
    FILE *out = fopen(output_filename, "wb");
    if (!out) {
        perror("Failed to open output file");
        free(data);
        return -1;
    }
    
    /* Write header */
    if (fwrite(&hdr, sizeof(hdr), 1, out) != 1) {
        perror("Failed to write header");
        fclose(out);
        free(data);
        return -1;
    }
    
    /* Write original data */
    if (fwrite(data, 1, file_size, out) != file_size) {
        perror("Failed to write data");
        fclose(out);
        free(data);
        return -1;
    }
    
    fclose(out);
    free(data);
    
    /* Replace original file */
    if (rename(output_filename, filename) != 0) {
        perror("Failed to rename output file");
        return -1;
    }
    
    printf("ZTS header injected into: %s\n", filename);
    printf("  Name: %s\n", hdr.name);
    printf("  UUID: ");
    print_uuid(hdr.uuid);
    printf("\n");
    printf("  Category: %s\n", category_to_string(category));
    printf("  Version: %d.%d.%d\n", hdr.major_version, hdr.minor_version, hdr.patch_version);
    printf("  Entry points: init=0x%lx, exec=0x%lx, cleanup=0x%lx\n",
           (unsigned long)hdr.init_func_offset,
           (unsigned long)hdr.execute_func_offset,
           (unsigned long)hdr.cleanup_func_offset);
    printf("  Checksum: 0x%08x\n", hdr.checksum);
    
    return 0;
}

/* ============================================================
 * Header Info Display
 * ============================================================ */

static int show_info(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("Failed to open file");
        return -1;
    }
    
    struct zts_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        fprintf(stderr, "Failed to read header\n");
        return -1;
    }
    fclose(f);
    
    /* Validate magic */
    if (hdr.magic != ZTS_MAGIC) {
        fprintf(stderr, "Invalid ZTS magic: 0x%08x (expected 0x%08x)\n",
                hdr.magic, ZTS_MAGIC);
        return -1;
    }
    
    printf("=== ZTS Module Info ===\n");
    printf("File: %s\n", filename);
    printf("Magic: 0x%08x\n", hdr.magic);
    printf("Version: %d (min: %d)\n", hdr.version, hdr.api_min_version);
    printf("Name: %s\n", hdr.name);
    printf("Description: %s\n", hdr.description);
    printf("Author: %s\n", hdr.author);
    printf("License: %s\n", hdr.license);
    printf("UUID: ");
    print_uuid(hdr.uuid);
    printf("\n");
    printf("Category: %s\n", category_to_string((enum zts_category)hdr.category));
    printf("Version: %d.%d.%d\n", hdr.major_version, hdr.minor_version, hdr.patch_version);
    printf("Dependencies: %d\n", hdr.num_dependencies);
    printf("Init offset: 0x%lx\n", (unsigned long)hdr.init_func_offset);
    printf("Exec offset: 0x%lx\n", (unsigned long)hdr.execute_func_offset);
    printf("Cleanup offset: 0x%lx\n", (unsigned long)hdr.cleanup_func_offset);
    printf("Data offset: 0x%lx\n", (unsigned long)hdr.data_offset);
    printf("Data size: %ld\n", (long)hdr.data_size);
    printf("Checksum: 0x%08x\n", hdr.checksum);
    
    return 0;
}

/* ============================================================
 * Header Verification
 * ============================================================ */

static int verify_file(const char *filename) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        perror("Failed to open file");
        return -1;
    }
    
    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    struct zts_header hdr;
    if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
        fclose(f);
        fprintf(stderr, "Failed to read header\n");
        return -1;
    }
    fclose(f);
    
    int errors = 0;
    
    /* Check magic */
    if (hdr.magic != ZTS_MAGIC) {
        fprintf(stderr, "[FAIL] Invalid magic: 0x%08x\n", hdr.magic);
        errors++;
    } else {
        printf("[PASS] Magic: 0x%08x\n", hdr.magic);
    }
    
    /* Check API version */
    if (hdr.version < hdr.api_min_version) {
        fprintf(stderr, "[FAIL] Version mismatch: %d < %d\n", hdr.version, hdr.api_min_version);
        errors++;
    } else {
        printf("[PASS] Version: %d >= %d\n", hdr.version, hdr.api_min_version);
    }
    
    /* Check name */
    if (strlen(hdr.name) == 0) {
        fprintf(stderr, "[FAIL] Empty module name\n");
        errors++;
    } else {
        printf("[PASS] Name: %s\n", hdr.name);
    }
    
    /* Verify checksum */
    uint32_t expected_checksum = hdr.checksum;
    hdr.checksum = 0;
    uint32_t actual_checksum = calculate_checksum((uint8_t*)&hdr, sizeof(hdr));
    if (actual_checksum != expected_checksum) {
        fprintf(stderr, "[FAIL] Checksum mismatch: 0x%08x != 0x%08x\n",
                actual_checksum, expected_checksum);
        errors++;
    } else {
        printf("[PASS] Checksum: 0x%08x\n", expected_checksum);
    }
    
    /* Check file size */
    if (file_size < (long)sizeof(hdr)) {
        fprintf(stderr, "[FAIL] File too small: %ld bytes\n", file_size);
        errors++;
    } else {
        printf("[PASS] File size: %ld bytes\n", file_size);
    }
    
    if (errors == 0) {
        printf("\n=== VERIFICATION PASSED ===\n");
        return 0;
    } else {
        fprintf(stderr, "\n=== VERIFICATION FAILED (%d errors) ===\n", errors);
        return -1;
    }
}

/* ============================================================
 * Main
 * ============================================================ */

static void usage(const char *progname) {
    printf("ZTS Header Tool - Inject/Inspect ZTS module headers\n\n");
    printf("Usage:\n");
    printf("  Inject header:\n");
    printf("    %s --name <name> --priority <hex> --category <cat> --inject <file.zts>\n\n", 
           progname);
    printf("  Show info:\n");
    printf("    %s --info <file.zts>\n\n", progname);
    printf("  Verify:\n");
    printf("    %s --verify <file.zts>\n\n", progname);
    printf("Options:\n");
    printf("  --name <name>       Module name\n");
    printf("  --priority <hex>    Load priority (hex, e.g., 0x00000001)\n");
    printf("  --category <cat>    Module category:\n");
    printf("                      FILESYSTEM, NETWORK, CRYPTO, SCHEDULER,\n");
    printf("                      MEMORY, DRIVER, PATCH, INJECT, CUSTOM\n");
    printf("  --inject <file>     Inject header into .zts file\n");
    printf("  --info <file>       Show module header info\n");
    printf("  --verify <file>     Verify module integrity\n");
    printf("  --help              Show this help\n");
}

int main(int argc, char *argv[]) {
    char *name = NULL;
    char *priority_str = "0x00000000";
    char *category_str = "CUSTOM";
    char *inject_file = NULL;
    char *info_file = NULL;
    char *verify_file_arg = NULL;
    
    static struct option long_options[] = {
        {"name",     required_argument, 0, 'n'},
        {"priority", required_argument, 0, 'p'},
        {"category", required_argument, 0, 'c'},
        {"inject",   required_argument, 0, 'i'},
        {"info",     required_argument, 0, 'I'},
        {"verify",   required_argument, 0, 'v'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "n:p:c:i:I:vh", long_options, NULL)) != -1) {
        switch (opt) {
        case 'n': name = optarg; break;
        case 'p': priority_str = optarg; break;
        case 'c': category_str = optarg; break;
        case 'i': inject_file = optarg; break;
        case 'I': info_file = optarg; break;
        case 'v': verify_file_arg = optarg; break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 1;
        }
    }
    
    /* Process commands */
    if (inject_file) {
        if (!name) {
            fprintf(stderr, "Error: --name required for inject mode\n");
            return 1;
        }
        
        uint32_t priority = (uint32_t)strtoul(priority_str, NULL, 0);
        enum zts_category category = string_to_category(category_str);
        
        return inject_header(inject_file, name, priority, category);
    }
    
    if (info_file) {
        return show_info(info_file);
    }
    
    if (verify_file_arg) {
        return verify_file(verify_file_arg);
    }
    
    /* No command specified */
    usage(argv[0]);
    return 1;
}
