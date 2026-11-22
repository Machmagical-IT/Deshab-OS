/* efi.h — 最小 UEFI 定义（无 gnu-efi / EDK2 依赖）
 *
 * 只声明 Deshab 引导器实际使用的协议：GOP、SimpleFileSystem、
 * LoadedImage、ACPI 配置表、内存映射、ExitBootServices。
 * 结构布局必须与 UEFI 2.10 规范逐字段一致（含保留槽位）。
 */
#ifndef DESHAB_BOOT_EFI_H
#define DESHAB_BOOT_EFI_H

typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;
typedef signed long long   i64;
typedef signed long long   s64;
typedef unsigned long long usize;

typedef u64 EFI_STATUS;
typedef void *EFI_HANDLE;
typedef void *EFI_EVENT;
typedef u64 EFI_TPL;
typedef u64 EFI_PHYSICAL_ADDRESS;
typedef u64 EFI_VIRTUAL_ADDRESS;
typedef u16 CHAR16;
typedef u8  BOOLEAN;

#define EFIAPI
#define EFI_SUCCESS 0
#define EFI_ERROR_BIT (1ULL << 63)
#define EFIERR(x) (EFI_ERROR_BIT | (x))
#define EFI_BUFFER_TOO_SMALL EFIERR(5)
#define EFI_NOT_FOUND EFIERR(14)
#define EFI_ABORTED EFIERR(21)

#define EFI_PAGE_SIZE 4096ULL

typedef struct {
    u32 Data1;
    u16 Data2;
    u16 Data3;
    u8  Data4[8];
} EFI_GUID;

typedef struct {
    u64 Signature;
    u32 Revision;
    u32 HeaderSize;
    u32 CRC32;
    u32 Reserved;
} EFI_TABLE_HEADER;

/* ---------------- 内存描述符 ---------------- */
typedef enum {
    EfiReservedMemoryType = 0,
    EfiLoaderCode = 1,
    EfiLoaderData = 2,
    EfiBootServicesCode = 3,
    EfiBootServicesData = 4,
    EfiRuntimeServicesCode = 5,
    EfiRuntimeServicesData = 6,
    EfiConventionalMemory = 7,
    EfiUnusableMemory = 8,
    EfiACPIReclaimMemory = 9,
    EfiACPIMemoryNVS = 10,
    EfiMemoryMappedIO = 11,
    EfiMemoryMappedIOPortSpace = 12,
    EfiPalCode = 13,
    EfiPersistentMemory = 14
} EFI_MEMORY_TYPE;

typedef enum {
    AllocateAnyPages = 0,
    AllocateMaxAddress = 1,
    AllocateAddress = 2,
    MaxAllocateType = 3
} EFI_ALLOCATE_TYPE;

typedef struct {
    u32 Type;
    u32 Pad;
    EFI_PHYSICAL_ADDRESS PhysicalStart;
    EFI_VIRTUAL_ADDRESS VirtualStart;
    u64 NumberOfPages;
    u64 Attribute;
} EFI_MEMORY_DESCRIPTOR;

/* ---------------- 文件协议 ---------------- */
typedef struct EFI_FILE_PROTOCOL EFI_FILE_PROTOCOL;

struct EFI_FILE_PROTOCOL {
    u64 Revision;
    EFI_STATUS (EFIAPI *Open)(EFI_FILE_PROTOCOL *This, EFI_FILE_PROTOCOL **NewHandle,
                              CHAR16 *FileName, u64 OpenMode, u64 Attributes);
    EFI_STATUS (EFIAPI *Close)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Delete)(EFI_FILE_PROTOCOL *This);
    EFI_STATUS (EFIAPI *Read)(EFI_FILE_PROTOCOL *This, usize *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Write)(EFI_FILE_PROTOCOL *This, usize *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *GetPosition)(EFI_FILE_PROTOCOL *This, u64 *Position);
    EFI_STATUS (EFIAPI *SetPosition)(EFI_FILE_PROTOCOL *This, u64 Position);
    EFI_STATUS (EFIAPI *GetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                 usize *BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *SetInfo)(EFI_FILE_PROTOCOL *This, EFI_GUID *InformationType,
                                 usize BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *Flush)(EFI_FILE_PROTOCOL *This);
};

typedef struct {
    u64 Revision;
    EFI_STATUS (EFIAPI *OpenVolume)(void *This, EFI_FILE_PROTOCOL **Root);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

typedef struct {
    u16 Year;
    u8  Month;
    u8  Day;
    u8  Hour;
    u8  Minute;
    u8  Second;
    u8  Pad1;
    u32 Nanosecond;
    short TimeZone;   /* EFI_TIME.TimeZone 为 INT16，整个结构固定 16 字节 */
    u8  Daylight;
    u8  Pad2;
} EFI_TIME;

typedef struct {
    u64 Size;
    u64 FileSize;
    u64 PhysicalSize;
    EFI_TIME CreateTime;
    EFI_TIME LastAccessTime;
    EFI_TIME ModificationTime;
    u64 Attribute;
    CHAR16 FileName[1];
} EFI_FILE_INFO;

#define EFI_FILE_MODE_READ 0x0000000000000001ULL
#define EFI_FILE_DIRECTORY 0x0000000000000010ULL

/* ---------------- 图形输出协议 ---------------- */
typedef struct {
    u32 RedMask;
    u32 GreenMask;
    u32 BlueMask;
    u32 ReservedMask;
} EFI_PIXEL_BITMASK;

typedef enum {
    PixelRedGreenBlueReserved8BitPerColor = 0,
    PixelBlueGreenRedReserved8BitPerColor = 1,
    PixelBitMask = 2,
    PixelBltOnly = 3
} EFI_GRAPHICS_PIXEL_FORMAT;

typedef struct {
    u32 Version;
    u32 HorizontalResolution;
    u32 VerticalResolution;
    EFI_GRAPHICS_PIXEL_FORMAT PixelFormat;
    EFI_PIXEL_BITMASK PixelInformation;
    u32 PixelsPerScanLine;
} EFI_GRAPHICS_OUTPUT_MODE_INFORMATION;

typedef struct {
    u32 MaxMode;
    u32 Mode;
    EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *Info;
    usize SizeOfInfo;
    EFI_PHYSICAL_ADDRESS FrameBufferBase;
    usize FrameBufferSize;
} EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE;

typedef struct {
    u32 Blue;
    u32 Green;
    u32 Red;
    u32 Reserved;
} EFI_GRAPHICS_OUTPUT_BLT_PIXEL;

typedef struct {
    EFI_STATUS (EFIAPI *QueryMode)(void *This, u32 ModeNumber, usize *SizeOfInfo,
                                   EFI_GRAPHICS_OUTPUT_MODE_INFORMATION **Info);
    EFI_STATUS (EFIAPI *SetMode)(void *This, u32 ModeNumber);
    EFI_STATUS (EFIAPI *Blt)(void *This, EFI_GRAPHICS_OUTPUT_BLT_PIXEL *BltBuffer,
                             u32 BltOperation, usize SourceX, usize SourceY,
                             usize DestinationX, usize DestinationY,
                             usize Width, usize Height, usize Delta);
    EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE *Mode;
} EFI_GRAPHICS_OUTPUT_PROTOCOL;

/* ---------------- BlockIo ---------------- */
typedef struct {
    u32 MediaId;
    u8  RemovableMedia;
    u8  MediaPresent;
    u8  LogicalPartition;
    u8  ReadOnly;
    u8  WriteCaching;
    u8  Pad[3];
    u32 IoAlign;
    u32 LastBlock;
    u32 LowestAlignedLba;
    u32 LogicalBlocksPerPhysicalBlock;
    u32 OptimalTransferLengthGranularity;
    u32 OptimalTransferLength;
} EFI_BLOCK_IO_MEDIA;

typedef struct {
    u64 Revision;
    EFI_BLOCK_IO_MEDIA *Media;
    EFI_STATUS (EFIAPI *Reset)(void *This, BOOLEAN ExtendedVerification);
    EFI_STATUS (EFIAPI *ReadBlocks)(void *This, u32 MediaId, u64 Lba,
                                    usize BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *WriteBlocks)(void *This, u32 MediaId, u64 Lba,
                                     usize BufferSize, void *Buffer);
    EFI_STATUS (EFIAPI *FlushBlocks)(void *This);
} EFI_BLOCK_IO_PROTOCOL;

/* ---------------- LoadedImage ---------------- */
typedef struct {
    u32 Revision;
    EFI_HANDLE ParentHandle;
    struct EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    u32 LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    u64 ImageSize;
    u32 ImageCodeType;
    u32 ImageDataType;
    EFI_STATUS (EFIAPI *Unload)(EFI_HANDLE ImageHandle);
} EFI_LOADED_IMAGE_PROTOCOL;

/* ---------------- 配置表 ---------------- */
typedef struct {
    EFI_GUID VendorGuid;
    void *VendorTable;
} EFI_CONFIGURATION_TABLE;

/* ---------------- 启动服务 ---------------- */
typedef struct {
    EFI_TABLE_HEADER Hdr;
    EFI_TPL (EFIAPI *RaiseTPL)(EFI_TPL NewTpl);
    void (EFIAPI *RestoreTPL)(EFI_TPL OldTpl);
    EFI_STATUS (EFIAPI *AllocatePages)(EFI_ALLOCATE_TYPE Type, EFI_MEMORY_TYPE MemoryType,
                                       usize Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS (EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, usize Pages);
    EFI_STATUS (EFIAPI *GetMemoryMap)(usize *MemoryMapSize, EFI_MEMORY_DESCRIPTOR *MemoryMap,
                                      usize *MapKey, usize *DescriptorSize, u32 *DescriptorVersion);
    EFI_STATUS (EFIAPI *AllocatePool)(EFI_MEMORY_TYPE PoolType, usize Size, void **Buffer);
    EFI_STATUS (EFIAPI *FreePool)(void *Buffer);
    EFI_STATUS (EFIAPI *CreateEvent)(u32 Type, EFI_TPL NotifyTpl, void *NotifyFunction,
                                     void *NotifyContext, EFI_EVENT *Event);
    EFI_STATUS (EFIAPI *SetTimer)(EFI_EVENT Event, u32 Type, u64 TriggerTime);
    EFI_STATUS (EFIAPI *WaitForEvent)(usize NumberOfEvents, EFI_EVENT *Event);
    EFI_STATUS (EFIAPI *SignalEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CloseEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *CheckEvent)(EFI_EVENT Event);
    EFI_STATUS (EFIAPI *InstallProtocolInterface)(EFI_HANDLE *Handle, EFI_GUID *Protocol,
                                                  u32 InterfaceType, void *Interface);
    EFI_STATUS (EFIAPI *ReinstallProtocolInterface)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                                    void *OldInterface, void *NewInterface);
    EFI_STATUS (EFIAPI *UninstallProtocolInterface)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                                    void *Interface);
    EFI_STATUS (EFIAPI *HandleProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface);
    void *Reserved;
    EFI_STATUS (EFIAPI *RegisterProtocolNotify)(EFI_GUID *Protocol, EFI_EVENT Event, void **Registration);
    EFI_STATUS (EFIAPI *LocateHandle)(u32 SearchType, EFI_GUID *Protocol, void *SearchKey,
                                      usize *BufferSize, EFI_HANDLE *Buffer);
    EFI_STATUS (EFIAPI *LocateDevicePath)(EFI_GUID *Protocol, void **DevicePath, EFI_HANDLE *Device);
    EFI_STATUS (EFIAPI *InstallConfigurationTable)(EFI_GUID *Guid, void *Table);
    EFI_STATUS (EFIAPI *LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE ParentImageHandle,
                                   void *DevicePath, void *SourceBuffer, usize SourceSize,
                                   EFI_HANDLE *ImageHandle);
    EFI_STATUS (EFIAPI *StartImage)(EFI_HANDLE ImageHandle, usize *ExitDataSize, CHAR16 **ExitData);
    EFI_STATUS (EFIAPI *Exit)(EFI_HANDLE ImageHandle, EFI_STATUS ExitStatus,
                              usize ExitDataSize, CHAR16 *ExitData);
    EFI_STATUS (EFIAPI *UnloadImage)(EFI_HANDLE ImageHandle);
    EFI_STATUS (EFIAPI *ExitBootServices)(EFI_HANDLE ImageHandle, usize MapKey);
    EFI_STATUS (EFIAPI *GetNextMonotonicCount)(u64 *Count);
    EFI_STATUS (EFIAPI *Stall)(usize Microseconds);
    EFI_STATUS (EFIAPI *SetWatchdogTimer)(usize Timeout, u64 WatchdogCode,
                                          usize DataSize, CHAR16 *WatchdogData);
    EFI_STATUS (EFIAPI *ConnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE *DriverImageHandle,
                                           void *RemainingDevicePath, BOOLEAN Recursive);
    EFI_STATUS (EFIAPI *DisconnectController)(EFI_HANDLE ControllerHandle, EFI_HANDLE DriverImageHandle,
                                              EFI_HANDLE ChildHandle);
    EFI_STATUS (EFIAPI *OpenProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol, void **Interface,
                                      EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle,
                                      u32 Attributes);
    EFI_STATUS (EFIAPI *CloseProtocol)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                       EFI_HANDLE AgentHandle, EFI_HANDLE ControllerHandle);
    EFI_STATUS (EFIAPI *OpenProtocolInformation)(EFI_HANDLE Handle, EFI_GUID *Protocol,
                                                 void **EntryBuffer, usize *EntryCount);
    EFI_STATUS (EFIAPI *ProtocolsPerHandle)(EFI_HANDLE Handle, EFI_GUID ***ProtocolBuffer,
                                            usize *ProtocolBufferCount);
    EFI_STATUS (EFIAPI *LocateHandleBuffer)(u32 SearchType, EFI_GUID *Protocol, void *SearchKey,
                                            usize *NoHandles, EFI_HANDLE **Buffer);
    EFI_STATUS (EFIAPI *LocateProtocol)(EFI_GUID *Protocol, void *Registration, void **Interface);
    EFI_STATUS (EFIAPI *InstallMultipleProtocolInterfaces)(EFI_HANDLE *Handle, ...);
    EFI_STATUS (EFIAPI *UninstallMultipleProtocolInterfaces)(EFI_HANDLE Handle, ...);
    EFI_STATUS (EFIAPI *CalculateCrc32)(void *Data, usize DataSize, u32 *Crc32);
    EFI_STATUS (EFIAPI *CopyMem)(void *Destination, void *Source, usize Length);
    EFI_STATUS (EFIAPI *SetMem)(void *Buffer, usize Size, u8 Value);
    EFI_STATUS (EFIAPI *CreateEventEx)(u32 Type, EFI_TPL NotifyTpl, void *NotifyFunction,
                                       void *NotifyContext, EFI_GUID *EventGroup, EFI_EVENT *Event);
} EFI_BOOT_SERVICES;

typedef struct EFI_SIMPLE_TEXT_INPUT_PROTOCOL EFI_SIMPLE_TEXT_INPUT_PROTOCOL;
typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

typedef struct EFI_SYSTEM_TABLE {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    u32 FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    EFI_SIMPLE_TEXT_INPUT_PROTOCOL *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *StdErr;
    void *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    usize NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

/* ---------------- 协议 GUID ---------------- */
#define GUID_INIT(a,b,c,d0,d1,d2,d3,d4,d5,d6,d7) \
    { (a), (b), (c), { (d0),(d1),(d2),(d3),(d4),(d5),(d6),(d7) } }

static const EFI_GUID EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID =
    GUID_INIT(0x9042a9de, 0x23dc, 0x4a38, 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a);
static const EFI_GUID EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID =
    GUID_INIT(0x964e5b22, 0x6459, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);
static const EFI_GUID EFI_LOADED_IMAGE_PROTOCOL_GUID =
    GUID_INIT(0x5b1b31a1, 0x9562, 0x11d2, 0x8e, 0x3f, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);
static const EFI_GUID EFI_BLOCK_IO_PROTOCOL_GUID =
    GUID_INIT(0x964e5b21, 0x6459, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);
static const EFI_GUID EFI_FILE_INFO_GUID =
    GUID_INIT(0x09576e92, 0x6d3f, 0x11d2, 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b);
static const EFI_GUID EFI_ACPI_20_TABLE_GUID =
    GUID_INIT(0x8868e871, 0xe4f1, 0x11d3, 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81);
static const EFI_GUID EFI_ACPI_10_TABLE_GUID =
    GUID_INIT(0xeb9d2d30, 0x2d88, 0x11d3, 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d);

#endif /* DESHAB_BOOT_EFI_H */
