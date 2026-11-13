/*
 * Minimal UEFI definitions for VtoyBoot (x86_64, MS ABI). Only what VtoyBoot uses.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define EFIAPI __attribute__((ms_abi))
typedef uint64_t UINTN;
typedef uint64_t EFI_STATUS;
typedef void *EFI_HANDLE;
typedef uint16_t CHAR16;
typedef uint8_t BOOLEAN;
typedef uint64_t EFI_PHYSICAL_ADDRESS;

#define EFI_ERR(x) (0x8000000000000000ULL | (x))
#define EFI_SUCCESS 0
#define EFI_INVALID_PARAMETER EFI_ERR(2)
#define EFI_UNSUPPORTED EFI_ERR(3)
#define EFI_BUFFER_TOO_SMALL EFI_ERR(5)
#define EFI_NOT_FOUND EFI_ERR(14)
#define EFI_ERROR(s) (((int64_t)(s)) < 0)

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } EFI_GUID;

typedef struct {
    uint64_t Signature;
    uint32_t Revision, HeaderSize, CRC32, Reserved;
} EFI_TABLE_HEADER;

typedef struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL {
    void *Reset;
    EFI_STATUS(EFIAPI *OutputString)(struct EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *, CHAR16 *);
} EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL;

enum { AllocateAnyPages, AllocateMaxAddress, AllocateAddress };
enum { EfiReservedMemoryType, EfiLoaderCode, EfiLoaderData, EfiBootServicesCode, EfiBootServicesData,
       EfiRuntimeServicesCode, EfiRuntimeServicesData, EfiConventionalMemory, EfiUnusableMemory,
       EfiACPIReclaimMemory, EfiACPIMemoryNVS };

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS(EFIAPI *AllocatePages)(int Type, int MemoryType, UINTN Pages, EFI_PHYSICAL_ADDRESS *Memory);
    EFI_STATUS(EFIAPI *FreePages)(EFI_PHYSICAL_ADDRESS Memory, UINTN Pages);
    void *GetMemoryMap;
    EFI_STATUS(EFIAPI *AllocatePool)(int PoolType, UINTN Size, void **Buffer);
    EFI_STATUS(EFIAPI *FreePool)(void *Buffer);
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS(EFIAPI *HandleProtocol)(EFI_HANDLE, EFI_GUID *, void **);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    EFI_STATUS(EFIAPI *LoadImage)(BOOLEAN BootPolicy, EFI_HANDLE Parent, void *DevicePath, void *Src, UINTN Size,
                                  EFI_HANDLE *Image);
    EFI_STATUS(EFIAPI *StartImage)(EFI_HANDLE Image, UINTN *ExitDataSize, CHAR16 **ExitData);
    void *Exit, *UnloadImage, *ExitBootServices, *GetNextMonotonicCount;
    EFI_STATUS(EFIAPI *Stall)(UINTN Microseconds);
    void *SetWatchdogTimer, *ConnectController, *DisconnectController, *OpenProtocol, *CloseProtocol,
        *OpenProtocolInformation, *ProtocolsPerHandle, *LocateHandleBuffer, *LocateProtocol,
        *InstallMultipleProtocolInterfaces, *UninstallMultipleProtocolInterfaces, *CalculateCrc32;
    void (EFIAPI *CopyMem)(void *Dst, const void *Src, UINTN Len);
    void (EFIAPI *SetMem)(void *Buf, UINTN Len, uint8_t Val);
} EFI_BOOT_SERVICES;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    void *GetTime, *SetTime, *GetWakeupTime, *SetWakeupTime, *SetVirtualAddressMap, *ConvertPointer;
    EFI_STATUS(EFIAPI *GetVariable)(CHAR16 *Name, EFI_GUID *Vendor, uint32_t *Attr, UINTN *Size, void *Data);
    void *GetNextVariableName;
    EFI_STATUS(EFIAPI *SetVariable)(CHAR16 *Name, EFI_GUID *Vendor, uint32_t Attr, UINTN Size, void *Data);
} EFI_RUNTIME_SERVICES;

typedef struct { EFI_GUID VendorGuid; void *VendorTable; } EFI_CONFIGURATION_TABLE;

typedef struct {
    EFI_TABLE_HEADER Hdr;
    CHAR16 *FirmwareVendor;
    uint32_t FirmwareRevision;
    EFI_HANDLE ConsoleInHandle;
    void *ConIn;
    EFI_HANDLE ConsoleOutHandle;
    EFI_SIMPLE_TEXT_OUTPUT_PROTOCOL *ConOut;
    EFI_HANDLE StandardErrorHandle;
    void *StdErr;
    EFI_RUNTIME_SERVICES *RuntimeServices;
    EFI_BOOT_SERVICES *BootServices;
    UINTN NumberOfTableEntries;
    EFI_CONFIGURATION_TABLE *ConfigurationTable;
} EFI_SYSTEM_TABLE;

typedef struct {
    uint32_t Revision;
    EFI_HANDLE ParentHandle;
    EFI_SYSTEM_TABLE *SystemTable;
    EFI_HANDLE DeviceHandle;
    void *FilePath;
    void *Reserved;
    uint32_t LoadOptionsSize;
    void *LoadOptions;
    void *ImageBase;
    uint64_t ImageSize;
    int ImageCodeType, ImageDataType;
    void *Unload;
} EFI_LOADED_IMAGE_PROTOCOL;

#define EFI_FILE_MODE_READ 0x1ULL
#define EFI_FILE_MODE_WRITE 0x2ULL
#define EFI_FILE_MODE_CREATE 0x8000000000000000ULL

typedef struct EFI_FILE_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS(EFIAPI *Open)(struct EFI_FILE_PROTOCOL *, struct EFI_FILE_PROTOCOL **, CHAR16 *, uint64_t, uint64_t);
    EFI_STATUS(EFIAPI *Close)(struct EFI_FILE_PROTOCOL *);
    EFI_STATUS(EFIAPI *Delete)(struct EFI_FILE_PROTOCOL *);
    EFI_STATUS(EFIAPI *Read)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    EFI_STATUS(EFIAPI *Write)(struct EFI_FILE_PROTOCOL *, UINTN *, void *);
    void *GetPosition, *SetPosition, *GetInfo, *SetInfo;
    EFI_STATUS(EFIAPI *Flush)(struct EFI_FILE_PROTOCOL *);
} EFI_FILE_PROTOCOL;

typedef struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL {
    uint64_t Revision;
    EFI_STATUS(EFIAPI *OpenVolume)(struct EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *, EFI_FILE_PROTOCOL **);
} EFI_SIMPLE_FILE_SYSTEM_PROTOCOL;

/* device path: we build MEDIA_FILEPATH appended to the image's device path */
typedef struct { uint8_t Type, SubType; uint8_t Length[2]; } EFI_DEVICE_PATH;
