/*
 * VtoyMac - see VtoyMac.hpp for the design notes.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "VtoyMac.hpp"

#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOSubMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <libkern/OSAtomic.h>
#include <libkern/OSByteOrder.h>
#include <pexpert/pexpert.h>

OSDefineMetaClassAndStructors(net_ventoy_VtoyMac, IOPartitionScheme)
#define super IOPartitionScheme

static const vtoy_guid kVentoyGuid = {
    0x77772020, 0x2e77, 0x6576, {0x6e, 0x74, 0x6f, 0x79, 0x2e, 0x6e, 0x65, 0x74}};

static constexpr UInt32 kMaxLocationTable = 16 * 1024 * 1024;

static OSData *copyNvram(const char *key)
{
    IORegistryEntry *opts = IORegistryEntry::fromPath("/options", gIODTPlane);
    if (!opts) {
        return nullptr;
    }
    OSObject *obj = opts->copyProperty(key);
    OSData *data = OSDynamicCast(OSData, obj);
    if (!data) {
        OSSafeReleaseNULL(obj);
    }
    opts->release();
    return data;
}

static bool endsWith(const char *s, const char *suffix)
{
    size_t ls = strnlen(s, 384), lx = strlen(suffix);
    return ls >= lx && strncmp(s + ls - lx, suffix, lx) == 0;
}

/* ------------------------------------------------------------------ extents */

void net_ventoy_VtoyMac::resetExtents()
{
    if (_ext) {
        IOFree(_ext, _extCap * sizeof(VtoyExtent));
    }
    _ext = nullptr;
    _extCount = _extCap = 0;
    _size = 0;
}

bool net_ventoy_VtoyMac::addExtent(UInt64 img, UInt64 disk, UInt64 len)
{
    if (len == 0) {
        return true;
    }
    if (_extCount > 0) {
        VtoyExtent &last = _ext[_extCount - 1];
        if (img != last.img + last.len) {
            VTOY_LOG("extent map has a hole at image offset %llu", img);
            return false;
        }
        if (disk == last.disk + last.len) { /* physically adjacent: merge */
            last.len += len;
            return true;
        }
    } else if (img != 0) {
        VTOY_LOG("extent map does not start at image offset 0");
        return false;
    }
    if (_extCount == _extCap) {
        UInt32 cap = _extCap ? _extCap * 2 : 256;
        VtoyExtent *n = (VtoyExtent *)IOMalloc(cap * sizeof(VtoyExtent));
        if (!n) {
            return false;
        }
        if (_ext) {
            memcpy(n, _ext, _extCount * sizeof(VtoyExtent));
            IOFree(_ext, _extCap * sizeof(VtoyExtent));
        }
        _ext = n;
        _extCap = cap;
    }
    _ext[_extCount++] = {img, disk, len};
    return true;
}

const VtoyExtent *net_ventoy_VtoyMac::findExtent(UInt64 img) const
{
    UInt32 lo = 0, hi = _extCount;
    while (lo < hi) {
        UInt32 mid = (lo + hi) / 2;
        if (img < _ext[mid].img) {
            hi = mid;
        } else if (img >= _ext[mid].img + _ext[mid].len) {
            lo = mid + 1;
        } else {
            return &_ext[mid];
        }
    }
    return nullptr;
}

/* ----------------------------------------------------------- probe helpers */

bool net_ventoy_VtoyMac::readDisk(IOMedia *media, UInt64 offset, void *dst, UInt32 len)
{
    UInt64 bs = media->getPreferredBlockSize();
    if (bs == 0) {
        bs = 512;
    }
    UInt64 start = offset - (offset % bs);
    UInt64 end = offset + len;
    UInt64 span = ((end + bs - 1) / bs) * bs - start;

    IOBufferMemoryDescriptor *buf = IOBufferMemoryDescriptor::withCapacity(span, kIODirectionIn);
    if (!buf) {
        return false;
    }
    UInt64 actual = 0;
    IOReturn rc = media->read(this, start, buf, nullptr, &actual);
    bool ok = (rc == kIOReturnSuccess && actual == span);
    if (ok) {
        memcpy(dst, (uint8_t *)buf->getBytesNoCopy() + (offset - start), len);
    } else {
        VTOY_LOG("read @%llu len %llu failed: 0x%x", start, span, rc);
    }
    buf->release();
    return ok;
}

/* Read from the image through the extent map (dst must not cross an extent). */
bool net_ventoy_VtoyMac::readImage(IOMedia *media, UInt64 offset, void *dst, UInt32 len)
{
    const VtoyExtent *e = findExtent(offset);
    if (!e || offset + len > e->img + e->len) {
        /* split across extents: do it sector by sector */
        for (UInt32 done = 0; done < len; done += 512) {
            const VtoyExtent *x = findExtent(offset + done);
            if (!x || !readDisk(media, x->disk + (offset + done - x->img), (uint8_t *)dst + done, 512)) {
                return false;
            }
        }
        return true;
    }
    return readDisk(media, e->disk + (offset - e->img), dst, len);
}

/*
 * The image must contain a GPT disk: "EFI PART" at LBA1. If expectGuid is
 * given the GPT disk GUID must match it too.
 */
bool net_ventoy_VtoyMac::validateImage(IOMedia *media, const uint8_t *expectGuid)
{
    uint8_t lba1[512];
    if (_size < 1024 * 1024) {
        VTOY_LOG("image too small (%llu)", _size);
        return false;
    }
    for (UInt32 i = 0; i < _extCount; i++) {
        if (_ext[i].disk + _ext[i].len > media->getSize()) {
            VTOY_LOG("extent %u outside the disk", i);
            return false;
        }
    }
    if (!readImage(media, 512, lba1, sizeof(lba1))) {
        return false;
    }
    if (memcmp(lba1, "EFI PART", 8) != 0) {
        VTOY_LOG("no GPT header inside the image");
        return false;
    }
    if (expectGuid && memcmp(lba1 + 56, expectGuid, 16) != 0) {
        VTOY_LOG("GPT disk GUID inside image does not match the map (stale vtoy-map?)");
        return false;
    }
    return true;
}

bool net_ventoy_VtoyMac::matchFromOsParam(IOMedia *media, const uint8_t *sector0)
{
    OSData *d = copyNvram(VTOY_OSPARAM_VAR);
    if (!d) {
        return false;
    }
    bool matched = false;
    uint8_t *tbl = nullptr;
    uint32_t len = 0;
    do {
        if (d->getLength() < sizeof(vtoy_os_param)) {
            VTOY_LOG("VentoyOsParam too short (%u)", d->getLength());
            break;
        }
        const vtoy_os_param *p = (const vtoy_os_param *)d->getBytesNoCopy();
        uint8_t sum = 0;
        for (size_t i = 0; i < sizeof(*p); i++) {
            sum += ((const uint8_t *)p)[i];
        }
        if (memcmp(&p->guid, &kVentoyGuid, sizeof(kVentoyGuid)) != 0 || sum != 0) {
            VTOY_LOG("VentoyOsParam has bad guid/checksum");
            break;
        }
        if (!endsWith(p->vtoy_img_path, ".vtoy")) {
            break; /* booted an ISO etc. - not a vdisk */
        }
        /* Is this the physical disk that holds the Ventoy partition? */
        if (memcmp(sector0 + 0x180, p->vtoy_disk_guid, 16) != 0 ||
            memcmp(sector0 + 0x1B8, p->vtoy_disk_signature, 4) != 0 ||
            media->getSize() != p->vtoy_disk_size) {
            break;
        }
        VTOY_LOG("Ventoy disk found, image %s (%llu bytes)", p->vtoy_img_path, p->vtoy_img_size);

        len = p->vtoy_img_location_len;
        if (p->vtoy_img_location_addr == 0 || len < sizeof(vtoy_image_location) || len > kMaxLocationTable) {
            VTOY_LOG("no usable image location table (addr=0x%llx len=%u)", p->vtoy_img_location_addr, len);
            break;
        }
        IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(p->vtoy_img_location_addr, len,
                                                                      kIODirectionIn, TASK_NULL);
        if (!md) {
            break;
        }
        tbl = (uint8_t *)IOMalloc(len);
        bool got = tbl && md->prepare() == kIOReturnSuccess;
        if (got) {
            got = md->readBytes(0, tbl, len) == len;
            md->complete();
        }
        md->release();
        if (!got) {
            VTOY_LOG("cannot read image location table at 0x%llx", p->vtoy_img_location_addr);
            break;
        }

        const vtoy_image_location *loc = (const vtoy_image_location *)tbl;
        uint64_t need = sizeof(vtoy_image_location) +
                        (uint64_t)(loc->region_count ? loc->region_count - 1 : 0) * sizeof(vtoy_image_disk_region);
        if (memcmp(&loc->guid, &kVentoyGuid, sizeof(kVentoyGuid)) != 0 || loc->region_count == 0 || need > len ||
            loc->disk_sector_size != 512 || (loc->image_sector_size != 512 && loc->image_sector_size != 2048)) {
            VTOY_LOG("bad location table (regions=%u imgsec=%u disksec=%u)", loc->region_count,
                     loc->image_sector_size, loc->disk_sector_size);
            break;
        }
        bool ok = true;
        for (uint32_t i = 0; i < loc->region_count && ok; i++) {
            const vtoy_image_disk_region *r = &loc->regions[i];
            ok = addExtent((UInt64)r->image_start_sector * loc->image_sector_size, r->disk_start_sector * 512ULL,
                           (UInt64)r->image_sector_count * loc->image_sector_size);
        }
        if (!ok || _extCount == 0) {
            break;
        }
        const VtoyExtent &last = _ext[_extCount - 1];
        UInt64 mapped = last.img + last.len;
        _size = p->vtoy_img_size < mapped ? p->vtoy_img_size : mapped;

        /* Fixed VHD: last 512 bytes are the "conectix" footer, not disk data. */
        uint8_t footer[512];
        if (_size > 512 && readImage(media, _size - 512, footer, sizeof(footer)) &&
            memcmp(footer, "conectix", 8) == 0) {
            if (OSReadBigInt32(footer, 60) != 2) {
                VTOY_LOG("VHD is not a fixed-size VHD");
                break;
            }
            _size -= 512;
        }
        _size &= ~511ULL;
        VTOY_LOG("%u region(s) -> %u extent(s), %llu bytes", loc->region_count, _extCount, _size);

        if (!validateImage(media, nullptr)) {
            break;
        }
        strlcpy(_source, "VentoyOsParam", sizeof(_source));
        matched = true;
    } while (false);
    if (tbl) {
        IOFree(tbl, len);
    }
    if (!matched) {
        resetExtents();
    }
    d->release();
    return matched;
}

bool net_ventoy_VtoyMac::matchFromMap(IOMedia *media, const uint8_t *sector0)
{
    OSData *d = copyNvram(VTOYMAC_MAP_VAR);
    if (!d) {
        return false;
    }
    bool matched = false;
    do {
        if (d->getLength() < sizeof(vtoymac_map)) {
            break;
        }
        const vtoymac_map *m = (const vtoymac_map *)d->getBytesNoCopy();
        if (memcmp(m->magic, "VTMP", 4) != 0 || m->version != 1) {
            break;
        }
        if (memcmp(sector0 + 0x1B8, m->disk_signature, 4) != 0 || media->getSize() != m->disk_size) {
            break;
        }
        static const uint8_t zero[16] = {0};
        if (memcmp(m->disk_guid, zero, 16) != 0 && memcmp(sector0 + 0x180, m->disk_guid, 16) != 0) {
            break;
        }
        if (!addExtent(0, m->start_lba * 512ULL, m->data_bytes & ~511ULL)) {
            break;
        }
        _size = m->data_bytes & ~511ULL;
        if (!validateImage(media, m->image_disk_guid)) {
            break;
        }
        strlcpy(_source, "vtoy-map", sizeof(_source));
        matched = true;
    } while (false);
    if (!matched) {
        resetExtents();
    }
    d->release();
    return matched;
}

/* ------------------------------------------------------------ IOService */

IOService *net_ventoy_VtoyMac::probe(IOService *provider, SInt32 *score)
{
    if (!super::probe(provider, score)) {
        return nullptr;
    }
    IOMedia *media = OSDynamicCast(IOMedia, provider);
    if (!media || !media->isWhole() || media->getPreferredBlockSize() != 512) {
        return nullptr;
    }
    if (PE_parse_boot_argn("-vtoymacoff", nullptr, 0)) {
        return nullptr;
    }
    if (!media->open(this, 0, kIOStorageAccessReader)) {
        return nullptr;
    }
    bool matched = false;
    uint8_t sector0[512];
    if (readDisk(media, 0, sector0, sizeof(sector0)) && sector0[510] == 0x55 && sector0[511] == 0xAA) {
        matched = matchFromOsParam(media, sector0) || matchFromMap(media, sector0);
    }
    media->close(this);

    if (!matched) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            OSData *a = copyNvram(VTOY_OSPARAM_VAR), *b = copyNvram(VTOYMAC_MAP_VAR);
            VTOY_LOG("nvram: VentoyOsParam %s, vtoy-map %s", a ? "present" : "absent", b ? "present" : "absent");
            OSSafeReleaseNULL(a);
            OSSafeReleaseNULL(b);
        }
        return nullptr;
    }
    VTOY_LOG("claiming %s: vdisk %llu bytes in %u extent(s) (source %s)", media->getName(), _size, _extCount, _source);
    return this;
}

bool net_ventoy_VtoyMac::start(IOService *provider)
{
    if (!super::start(provider)) {
        return false;
    }
    IOMedia *media = getProvider();

    _vdisk = new IOMedia;
    if (!_vdisk) {
        return false;
    }
    /* base 0: offsets reaching read()/write() are image offsets, we translate them. */
    if (!_vdisk->init(0, _size, 512, media->getAttributes(), true /* whole */, media->isWritable(), nullptr)) {
        _vdisk->release();
        _vdisk = nullptr;
        return false;
    }
    _vdisk->setName("Ventoy VDisk");
    _vdisk->setLocation("1");
    _vdisk->setProperty("VtoyMac Source", _source);
    _vdisk->setProperty("VtoyMac Extents", _extCount, 32);

    if (!_vdisk->attach(this)) {
        _vdisk->release();
        _vdisk = nullptr;
        return false;
    }
    _vdisk->registerService();
    registerService();
    VTOY_LOG("published Ventoy VDisk");
    return true;
}

void net_ventoy_VtoyMac::stop(IOService *provider)
{
    if (_vdisk) {
        _vdisk->detach(this);
    }
    super::stop(provider);
}

void net_ventoy_VtoyMac::free()
{
    OSSafeReleaseNULL(_vdisk);
    resetExtents();
    super::free();
}

/* ------------------------------------------------------------------- I/O */

namespace {
struct SplitIO {
    IOStorageCompletion orig;
    volatile SInt32 pending;
    volatile SInt32 status;
    volatile SInt64 actual;
};

void splitDone(void *target, void *parameter, IOReturn status, UInt64 actualByteCount)
{
    SplitIO *ctx = (SplitIO *)target;
    IOMemoryDescriptor *sub = (IOMemoryDescriptor *)parameter;
    if (sub) {
        sub->release();
    }
    if (status != kIOReturnSuccess) {
        OSCompareAndSwap(kIOReturnSuccess, (UInt32)status, (volatile UInt32 *)&ctx->status);
    }
    OSAddAtomic64((SInt64)actualByteCount, &ctx->actual);
    if (OSDecrementAtomic(&ctx->pending) == 1) {
        IOStorageCompletion orig = ctx->orig;
        IOReturn st = (IOReturn)ctx->status;
        UInt64 act = (UInt64)ctx->actual;
        IOFree(ctx, sizeof(*ctx));
        IOStorage::complete(&orig, st, act);
    }
}
} // namespace

void net_ventoy_VtoyMac::doIO(bool write, IOService *client, UInt64 byteStart, IOMemoryDescriptor *buffer,
                              IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
    IOMedia *media = getProvider();
    UInt64 length = buffer->getLength();

    if (byteStart + length > _size) {
        IOStorage::complete(completion, kIOReturnBadArgument, 0);
        return;
    }
    const VtoyExtent *e = findExtent(byteStart);
    if (!e) {
        IOStorage::complete(completion, kIOReturnBadArgument, 0);
        return;
    }
    /* Fast path: request inside one extent. */
    if (byteStart + length <= e->img + e->len) {
        UInt64 disk = e->disk + (byteStart - e->img);
        if (write) {
            media->write(this, disk, buffer, attributes, completion);
        } else {
            media->read(this, disk, buffer, attributes, completion);
        }
        return;
    }

    SplitIO *ctx = (SplitIO *)IOMalloc(sizeof(SplitIO));
    if (!ctx) {
        IOStorage::complete(completion, kIOReturnNoMemory, 0);
        return;
    }
    ctx->orig = *completion;
    ctx->pending = 1; /* guard until every piece is issued */
    ctx->status = kIOReturnSuccess;
    ctx->actual = 0;

    UInt64 done = 0;
    while (done < length) {
        UInt64 pos = byteStart + done;
        const VtoyExtent *x = findExtent(pos);
        if (!x) {
            OSCompareAndSwap(kIOReturnSuccess, (UInt32)kIOReturnBadArgument, (volatile UInt32 *)&ctx->status);
            break;
        }
        UInt64 n = x->img + x->len - pos;
        if (n > length - done) {
            n = length - done;
        }
        IOMemoryDescriptor *sub = IOSubMemoryDescriptor::withSubRange(buffer, done, n, buffer->getDirection());
        if (!sub) {
            OSCompareAndSwap(kIOReturnSuccess, (UInt32)kIOReturnNoMemory, (volatile UInt32 *)&ctx->status);
            break;
        }
        OSIncrementAtomic(&ctx->pending);
        IOStorageCompletion c = {ctx, splitDone, sub};
        UInt64 disk = x->disk + (pos - x->img);
        if (write) {
            media->write(this, disk, sub, attributes, &c);
        } else {
            media->read(this, disk, sub, attributes, &c);
        }
        done += n;
    }
    splitDone(ctx, nullptr, kIOReturnSuccess, 0); /* drop the guard */
}

void net_ventoy_VtoyMac::read(IOService *client, UInt64 byteStart, IOMemoryDescriptor *buffer,
                              IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
    doIO(false, client, byteStart, buffer, attributes, completion);
}

void net_ventoy_VtoyMac::write(IOService *client, UInt64 byteStart, IOMemoryDescriptor *buffer,
                               IOStorageAttributes *attributes, IOStorageCompletion *completion)
{
    doIO(true, client, byteStart, buffer, attributes, completion);
}

IOReturn net_ventoy_VtoyMac::synchronize(IOService *client, UInt64 byteStart, UInt64 byteCount,
                                         IOStorageSynchronizeOptions options)
{
    /* Ranges are image offsets; flush the whole physical disk instead. */
    return getProvider()->synchronize(this, 0, 0, options);
}

IOReturn net_ventoy_VtoyMac::unmap(IOService *client, IOStorageExtent *extents, UInt32 extentsCount,
                                   IOStorageUnmapOptions options)
{
    /* Never pass TRIM down: the sectors belong to a file on the host filesystem. */
    return kIOReturnSuccess;
}

IOReturn net_ventoy_VtoyMac::getProvisionStatus(IOService *client, UInt64 byteStart, UInt64 byteCount,
                                                UInt32 *extentsCount, IOStorageProvisionExtent *extents,
                                                IOStorageGetProvisionStatusOptions options)
{
    return kIOReturnUnsupported;
}

IOStorage *net_ventoy_VtoyMac::copyPhysicalExtent(IOService *client, UInt64 *byteStart, UInt64 *byteCount)
{
    const VtoyExtent *e = findExtent(*byteStart);
    if (!e) {
        return nullptr;
    }
    UInt64 avail = e->img + e->len - *byteStart;
    if (*byteCount > avail) {
        *byteCount = avail;
    }
    *byteStart = e->disk + (*byteStart - e->img);
    return getProvider()->copyPhysicalExtent(this, byteStart, byteCount);
}

IOReturn net_ventoy_VtoyMac::setPriority(IOService *client, IOStorageExtent *extents, UInt32 extentsCount,
                                         IOStoragePriority priority)
{
    return kIOReturnUnsupported;
}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
IOReturn net_ventoy_VtoyMac::discard(IOService *client, UInt64 byteStart, UInt64 byteCount)
{
    return kIOReturnSuccess;
}
#pragma clang diagnostic pop
