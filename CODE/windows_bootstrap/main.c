#include "../boot/efi.h"

#define ACPI_RSDP_GPA 0x000E0000ULL
#define COM1 0x3F8

void *memset(void *dst, int value, usize size) {
    u8 *p = (u8 *)dst;
    for (usize i = 0; i < size; i++) p[i] = (u8)value;
    return dst;
}

void *memcpy(void *dst, const void *src, usize size) {
    u8 *d = (u8 *)dst;
    const u8 *s = (const u8 *)src;
    for (usize i = 0; i < size; i++) d[i] = s[i];
    return dst;
}

static inline void outb(u16 port, u8 value) {
    __asm__ volatile("outb %0, %1" :: "a"(value), "Nd"(port));
}

static inline u8 inb(u16 port) {
    u8 value;
    __asm__ volatile("inb %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}

static void serial_putc(char c) {
    for (u32 i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++) {}
    outb(COM1, (u8)c);
}

static void serial_write(const char *text) {
    while (*text) {
        if (*text == '\n') serial_putc('\r');
        serial_putc(*text++);
    }
}

static void serial_hex(const char *label, u64 value) {
    serial_write(label);
    /* 不查表：全部算术生成，避免依赖 .rdata 里的常量数组。 */
    for (int shift = 60; shift >= 0; shift -= 4) {
        u32 v = (u32)((value >> shift) & 0xF);
        serial_putc((char)(v < 10 ? ('0' + (char)v) : ('a' + (char)(v - 10))));
    }
    serial_write("\n");
}

static void serial_status(const char *label, EFI_STATUS status) {
    serial_hex(label, (u64)status);
}

static const EFI_GUID EFI_DEVICE_PATH_PROTOCOL_GUID =
    GUID_INIT(0x09576e91, 0x6d3f, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);

typedef struct {
    u8 type;
    u8 subtype;
    u16 length;
} efi_devpath;

static usize devpath_prefix(const efi_devpath *node) {
    usize total = 0;
    while (node->type != 0x7F) {
        if (node->length < 4) return 0;
        total += node->length;
        node = (const efi_devpath *)((const u8 *)node + node->length);
    }
    return total;
}

/* DeviceHandle 的路径加上 bootmgfw 文件节点。LoadImage 需要这条路径，
 * 否则引导器不知道自己从哪块盘启动，读不到 BCD。 */
static void *bootmgfw_device_path(EFI_BOOT_SERVICES *bs, EFI_HANDLE device) {
    efi_devpath *device_path = 0;
    EFI_STATUS status = bs->HandleProtocol(
        device, (EFI_GUID *)&EFI_DEVICE_PATH_PROTOCOL_GUID, (void **)&device_path);
    if (status != EFI_SUCCESS || !device_path) return 0;
    usize prefix = devpath_prefix(device_path);
    if (!prefix) return 0;

    static const CHAR16 file[] = {
        '\\','E','F','I','\\','M','i','c','r','o','s','o','f','t','\\',
        'B','o','o','t','\\','b','o','o','t','m','g','f','w','.','e','f','i',0
    };
    usize chars = 0;
    while (file[chars]) chars++;
    usize node = 4 + (chars + 1) * 2;
    u8 *path = 0;
    status = bs->AllocatePool(EfiLoaderData, prefix + node + 4, (void **)&path);
    if (status != EFI_SUCCESS) return 0;
    memcpy(path, device_path, prefix);
    efi_devpath *file_node = (efi_devpath *)(path + prefix);
    serial_write("[WINBOOT] devpath");
    for (const efi_devpath *node = device_path; node->type != 0x7F; ) {
        static const char hex[] = "0123456789abcdef";
        serial_putc(' ');
        serial_putc(hex[node->type >> 4]);
        serial_putc(hex[node->type & 0xF]);
        serial_putc(':');
        serial_putc(hex[node->subtype >> 4]);
        serial_putc(hex[node->subtype & 0xF]);
        if (node->length < 4) break;
        node = (const efi_devpath *)((const u8 *)node + node->length);
    }
    serial_write("\n");
    file_node->type = 4;
    file_node->subtype = 4;
    file_node->length = (u16)node;
    memcpy(path + prefix + 4, file, (chars + 1) * 2);
    efi_devpath *end = (efi_devpath *)(path + prefix + node);
    end->type = 0x7F;
    end->subtype = 0xFF;
    end->length = 4;
    return path;
}

static EFI_STATUS read_boot_manager(EFI_HANDLE image, EFI_SYSTEM_TABLE *st,
                                    void **buffer_out, usize *size_out) {
    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    EFI_STATUS status = st->BootServices->HandleProtocol(
        image, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded);
    if (status != EFI_SUCCESS || !loaded) return status;

    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = 0;
    status = st->BootServices->HandleProtocol(
        loaded->DeviceHandle, (EFI_GUID *)&EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID,
        (void **)&fs);
    if (status != EFI_SUCCESS || !fs) return status;

    EFI_FILE_PROTOCOL *root = 0;
    status = fs->OpenVolume(fs, &root);
    if (status != EFI_SUCCESS || !root) return status;

    static CHAR16 path[] = {
        '\\','E','F','I','\\','M','i','c','r','o','s','o','f','t','\\',
        'B','o','o','t','\\','b','o','o','t','m','g','f','w','.','e','f','i',0
    };
    EFI_FILE_PROTOCOL *file = 0;
    status = root->Open(root, &file, path, EFI_FILE_MODE_READ, 0);
    if (status != EFI_SUCCESS || !file) {
        root->Close(root);
        return status;
    }

    usize info_size = 0;
    status = file->GetInfo(file, (EFI_GUID *)&EFI_FILE_INFO_GUID,
                           &info_size, 0);
    if (status != EFI_BUFFER_TOO_SMALL || info_size < sizeof(EFI_FILE_INFO)) {
        file->Close(file);
        root->Close(root);
        return status;
    }
    EFI_FILE_INFO *info = 0;
    status = st->BootServices->AllocatePool(EfiLoaderData, info_size,
                                             (void **)&info);
    if (status != EFI_SUCCESS) {
        file->Close(file);
        root->Close(root);
        return status;
    }
    status = file->GetInfo(file, (EFI_GUID *)&EFI_FILE_INFO_GUID,
                           &info_size, info);
    if (status != EFI_SUCCESS || !info->FileSize) {
        st->BootServices->FreePool(info);
        file->Close(file);
        root->Close(root);
        return status;
    }

    void *buffer = 0;
    usize file_size = (usize)info->FileSize;
    st->BootServices->FreePool(info);
    status = st->BootServices->AllocatePool(EfiLoaderData, file_size, &buffer);
    if (status == EFI_SUCCESS)
        status = file->Read(file, &file_size, buffer);
    file->Close(file);
    root->Close(root);
    if (status != EFI_SUCCESS) {
        if (buffer) st->BootServices->FreePool(buffer);
        return status;
    }
    *buffer_out = buffer;
    *size_out = file_size;
    return EFI_SUCCESS;
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *st) {
    serial_write("[WINBOOT] Deshab ACPI bridge\n");
    EFI_STATUS status = st->BootServices->InstallConfigurationTable(
        (EFI_GUID *)&EFI_ACPI_20_TABLE_GUID, (void *)ACPI_RSDP_GPA);
    if (status != EFI_SUCCESS) {
        serial_write("[WINBOOT] WIN-E25 ACPI install failed\n");
        return EFI_ABORTED;
    }

    void *boot_manager = 0;
    usize boot_manager_size = 0;
    status = read_boot_manager(image, st, &boot_manager, &boot_manager_size);
    if (status != EFI_SUCCESS) {
        serial_write("[WINBOOT] WIN-E26 bootmgfw read failed\n");
        return EFI_ABORTED;
    }

    EFI_LOADED_IMAGE_PROTOCOL *loaded = 0;
    status = st->BootServices->HandleProtocol(
        image, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID, (void **)&loaded);
    void *device_path = (status == EFI_SUCCESS && loaded)
        ? bootmgfw_device_path(st->BootServices, loaded->DeviceHandle) : 0;

    EFI_HANDLE child = 0;
    if (device_path) {
        status = st->BootServices->LoadImage(0, image, device_path, 0, 0, &child);
        serial_status("[WINBOOT] path load ", status);
    }
    if (!device_path || status != EFI_SUCCESS || !child) {
        status = st->BootServices->LoadImage(0, image, 0, boot_manager,
                                             boot_manager_size, &child);
        serial_status("[WINBOOT] buffer load ", status);
    }
    if (status != EFI_SUCCESS || !child) {
        serial_write("[WINBOOT] WIN-E27 bootmgfw LoadImage failed\n");
        return EFI_ABORTED;
    }
    /* 打印子映像的加载信息：确认 bootmgfw 到底被装到哪、多大。 */
    {
        EFI_LOADED_IMAGE_PROTOCOL *cli = 0;
        if (st->BootServices->HandleProtocol(
                child, (EFI_GUID *)&EFI_LOADED_IMAGE_PROTOCOL_GUID,
                (void **)&cli) == EFI_SUCCESS && cli) {
            u64 *raw = (u64 *)cli;      /* ImageBase @ +64, ImageSize @ +72 */
            serial_hex("[WINBOOT] child base ", raw[8]);
            serial_hex("[WINBOOT] child size ", raw[9]);
        } else {
            serial_write("[WINBOOT] child loaded-image unavailable\n");
        }
    }
    serial_write("[WINBOOT] starting Microsoft Boot Manager\n");
    usize exit_size = 0;
    CHAR16 *exit_data = 0;
    status = st->BootServices->StartImage(child, &exit_size, &exit_data);
    serial_status("[WINBOOT] WIN-E28 boot manager returned ", status);
    return status;
}
