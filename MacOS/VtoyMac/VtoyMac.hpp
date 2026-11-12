/*
 * VtoyMac - boot macOS from a Ventoy vdisk (*.vhd.vtoy / *.img.vtoy)
 *
 * Ventoy (UEFI, vtoyboot mode) exposes the vdisk file to firmware as a block
 * device, so OpenCore + boot.efi can load the kernel from it. Once XNU is
 * running, nothing knows where that file lives. This kext is the macOS
 * counterpart of Ventoy's Linux "vtoyboot" (which uses dm-linear): it attaches
 * to the physical disk that holds the Ventoy partition, builds the
 * image-offset -> disk-offset extent map of the vdisk file and publishes the
 * image as a whole-disk IOMedia ("Ventoy VDisk"). I/O on that media is
 * translated extent by extent (fragmented files are supported). The stock
 * GUID partition scheme and APFS then take it from there, and root mounts.
 *
 * Location sources, tried in order:
 *  1. EFI variable  VentoyOsParam (GUID 77772020-2E77-6576-6E74-6F792E6E6574)
 *     set by Ventoy, plus the image-location table it leaves in
 *     EfiRuntimeServicesData memory.
 *  2. NVRAM variable vtoy-map (GUID 6E0A4F2B-3C1D-4E5F-9A8B-7C6D5E4F3A21)
 *     written by OpenCore (NVRAM -> Add), generated on Windows by
 *     tools/vtoy-map.ps1 (single-extent files only).
 *
 * Every candidate is validated (disk identity + GPT header inside the image)
 * before anything is published; on any mismatch the kext declines and the
 * normal partition scheme drivers handle the disk.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later (same as Ventoy)
 */
#pragma once

#include <IOKit/storage/IOPartitionScheme.h>
#include <IOKit/storage/IOMedia.h>

#define VTOY_LOG(fmt, ...) IOLog("VtoyMac: " fmt "\n", ##__VA_ARGS__)

#pragma pack(1)
struct vtoy_guid {
    uint32_t data1;
    uint16_t data2;
    uint16_t data3;
    uint8_t  data4[8];
};

struct vtoy_image_disk_region {
    uint32_t image_sector_count;
    uint32_t image_start_sector;
    uint64_t disk_start_sector;
};

struct vtoy_image_location {
    vtoy_guid guid;
    uint32_t  image_sector_size;
    uint32_t  disk_sector_size;
    uint32_t  region_count;
    vtoy_image_disk_region regions[1];
};

struct vtoy_os_param {
    vtoy_guid guid;
    uint8_t   chksum;
    uint8_t   vtoy_disk_guid[16];
    uint64_t  vtoy_disk_size;
    uint16_t  vtoy_disk_part_id;
    uint16_t  vtoy_disk_part_type;
    char      vtoy_img_path[384];
    uint64_t  vtoy_img_size;
    uint64_t  vtoy_img_location_addr;
    uint32_t  vtoy_img_location_len;
    uint64_t  vtoy_reserved[4];
    uint8_t   vtoy_disk_signature[4];
    uint8_t   reserved[27];
};

/* Fallback map written by tools/vtoy-map.ps1 into OpenCore NVRAM->Add. */
struct vtoymac_map {
    char     magic[4];          /* "VTMP" */
    uint32_t version;           /* 1 */
    uint8_t  disk_signature[4]; /* MBR signature @0x1B8 of the physical disk */
    uint8_t  disk_guid[16];     /* Ventoy disk GUID @0x180, or all zero = don't check */
    uint64_t disk_size;         /* bytes */
    uint64_t start_lba;         /* first 512-byte sector of the image on disk */
    uint64_t data_bytes;        /* bytes of disk data (VHD footer excluded) */
    uint8_t  image_disk_guid[16]; /* GPT disk GUID inside the image */
};
#pragma pack()

static_assert(sizeof(vtoy_os_param) == 512, "ventoy_os_param must be 512 bytes");

#define VTOY_OSPARAM_VAR "77772020-2E77-6576-6E74-6F792E6E6574:VentoyOsParam"
#define VTOYMAC_MAP_VAR  "6E0A4F2B-3C1D-4E5F-9A8B-7C6D5E4F3A21:vtoy-map"

struct VtoyExtent {
    UInt64 img;   /* byte offset inside the image */
    UInt64 disk;  /* byte offset on the physical disk */
    UInt64 len;   /* bytes */
};

class net_ventoy_VtoyMac : public IOPartitionScheme {
    OSDeclareDefaultStructors(net_ventoy_VtoyMac)

    IOMedia    *_vdisk = nullptr;
    VtoyExtent *_ext = nullptr;
    UInt32      _extCount = 0;
    UInt32      _extCap = 0;
    UInt64      _size = 0;
    char        _source[16] = {0};

    bool addExtent(UInt64 img, UInt64 disk, UInt64 len);
    void resetExtents();
    const VtoyExtent *findExtent(UInt64 img) const;
    bool readDisk(IOMedia *media, UInt64 offset, void *dst, UInt32 len);
    bool readImage(IOMedia *media, UInt64 offset, void *dst, UInt32 len);
    bool matchFromOsParam(IOMedia *media, const uint8_t *sector0);
    bool matchFromMap(IOMedia *media, const uint8_t *sector0);
    bool validateImage(IOMedia *media, const uint8_t *expectGuid);
    void doIO(bool write, IOService *client, UInt64 byteStart, IOMemoryDescriptor *buffer,
              IOStorageAttributes *attributes, IOStorageCompletion *completion);

public:
    IOService *probe(IOService *provider, SInt32 *score) override;
    bool start(IOService *provider) override;
    void stop(IOService *provider) override;
    void free() override;

    void read(IOService *client, UInt64 byteStart, IOMemoryDescriptor *buffer,
              IOStorageAttributes *attributes, IOStorageCompletion *completion) override;
    void write(IOService *client, UInt64 byteStart, IOMemoryDescriptor *buffer,
               IOStorageAttributes *attributes, IOStorageCompletion *completion) override;
    IOReturn synchronize(IOService *client, UInt64 byteStart, UInt64 byteCount,
                         IOStorageSynchronizeOptions options) override;
    IOReturn unmap(IOService *client, IOStorageExtent *extents, UInt32 extentsCount,
                   IOStorageUnmapOptions options) override;
    IOReturn getProvisionStatus(IOService *client, UInt64 byteStart, UInt64 byteCount,
                                UInt32 *extentsCount, IOStorageProvisionExtent *extents,
                                IOStorageGetProvisionStatusOptions options) override;
    IOStorage *copyPhysicalExtent(IOService *client, UInt64 *byteStart, UInt64 *byteCount) override;
    IOReturn setPriority(IOService *client, IOStorageExtent *extents, UInt32 extentsCount,
                         IOStoragePriority priority) override;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    IOReturn discard(IOService *client, UInt64 byteStart, UInt64 byteCount) override;
#pragma clang diagnostic pop
};
