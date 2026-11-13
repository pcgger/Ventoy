/*
 * VtoyBoot - \EFI\BOOT\BOOTx64.efi of a macOS Ventoy vdisk. Runs before OpenCore on every boot:
 *
 *  1. identifies the machine (SMBIOS system/board vendor+product, CPUID vendor) and picks an OpenCore profile from
 *     \EFI\VTOY\profiles.txt (first matching line wins);
 *  2. if the image was started by Ventoy, copies VentoyOsParam (EFI variable) and Ventoy's image location table into
 *     ACPI NVS memory - which macOS never reclaims - and hands its address to VtoyMac.kext as "vtoyparam=0x...";
 *  3. writes \EFI\OC\config.plist from \EFI\VTOY\profiles\<profile>[-nvidia].plist, replacing
 *        @VTOYARGS@                 -> the generated boot arguments (may be empty)
 *        @CORES:<hexpre>:<hexsuf>@  -> base64(pre + physical core count byte + suf)  (AMD core count patches)
 *     the -nvidia variant is used when \EFI\VTOY\nvidia-installed exists (set by the NVIDIA auto-installer);
 *  4. starts \EFI\OC\OpenCore.efi.
 * Everything it decided is written to \EFI\VTOY\vtoyboot.log. On any failure it still starts OpenCore with whatever
 * config.plist is already there.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "efi.h"

static EFI_SYSTEM_TABLE *ST;
static EFI_BOOT_SERVICES *BS;

/* ---------------------------------------------------------------- libc bits the compiler may call */
void *memcpy(void *d, const void *s, size_t n) { uint8_t *a = d; const uint8_t *b = s; while (n--) *a++ = *b++; return d; }
void *memset(void *d, int c, size_t n) { uint8_t *a = d; while (n--) *a++ = (uint8_t)c; return d; }
int memcmp(const void *x, const void *y, size_t n)
{
    const uint8_t *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
static size_t slen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
static char lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; }

static const EFI_GUID gLoadedImageGuid = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static const EFI_GUID gFsGuid = {0x964e5b22, 0x6459, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID gDevicePathGuid = {0x09576e91, 0x6d3f, 0x11d2, {0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b}};
static const EFI_GUID gSmbios3Guid = {0xf2fd1544, 0x9794, 0x4a2c, {0x99, 0x2e, 0xe5, 0xbb, 0xcf, 0x20, 0xe3, 0x94}};
static const EFI_GUID gSmbiosGuid = {0xeb9d2d31, 0x2d88, 0x11d3, {0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d}};
static const EFI_GUID gVentoyGuid = {0x77772020, 0x2E77, 0x6576, {0x6E, 0x74, 0x6F, 0x79, 0x2E, 0x6E, 0x65, 0x74}};

/* ---------------------------------------------------------------- growable text buffer (log, config) */
typedef struct { char *p; UINTN len, cap; } Buf;
static void buf_put(Buf *b, const char *s, UINTN n)
{
    if (b->len + n + 1 > b->cap) {
        UINTN cap = (b->len + n + 1) * 2 + 256;
        char *np = NULL;
        if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, cap, (void **)&np))) return;
        if (b->p) { memcpy(np, b->p, b->len); BS->FreePool(b->p); }
        b->p = np; b->cap = cap;
    }
    memcpy(b->p + b->len, s, n);
    b->len += n;
    b->p[b->len] = 0;
}
static void buf_str(Buf *b, const char *s) { buf_put(b, s, slen(s)); }
static void buf_hex(Buf *b, uint64_t v)
{
    char t[19] = "0x"; int n = 2;
    for (int sh = 60; sh >= 0; sh -= 4) {
        int d = (v >> sh) & 15;
        if (d || n > 2 || sh == 0) t[n++] = "0123456789abcdef"[d];
    }
    buf_put(b, t, (UINTN)n);
}
static void buf_dec(Buf *b, uint64_t v)
{
    char t[21]; int n = 20; t[n] = 0;
    do { t[--n] = (char)('0' + v % 10); v /= 10; } while (v);
    buf_str(b, t + n);
}
static Buf gLog;

/* ---------------------------------------------------------------- files on the boot volume */
static EFI_FILE_PROTOCOL *gRoot;

static void to16(const char *s, CHAR16 *d) { while ((*d++ = (CHAR16)*s++)); }

static int read_file(const char *path, Buf *out)
{
    CHAR16 p[260]; EFI_FILE_PROTOCOL *f;
    to16(path, p);
    if (EFI_ERROR(gRoot->Open(gRoot, &f, p, EFI_FILE_MODE_READ, 0))) return 0;
    char chunk[4096];
    for (;;) {
        UINTN n = sizeof(chunk);
        if (EFI_ERROR(f->Read(f, &n, chunk)) || n == 0) break;
        buf_put(out, chunk, n);
    }
    f->Close(f);
    return out->p != NULL;
}

static int file_exists(const char *path)
{
    CHAR16 p[260]; EFI_FILE_PROTOCOL *f;
    to16(path, p);
    if (EFI_ERROR(gRoot->Open(gRoot, &f, p, EFI_FILE_MODE_READ, 0))) return 0;
    f->Close(f);
    return 1;
}

static int write_file(const char *path, const char *data, UINTN len)
{
    CHAR16 p[260]; EFI_FILE_PROTOCOL *f;
    to16(path, p);
    /* truncate by delete + create */
    if (!EFI_ERROR(gRoot->Open(gRoot, &f, p, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE, 0))) f->Delete(f);
    if (EFI_ERROR(gRoot->Open(gRoot, &f, p, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0)))
        return 0;
    UINTN n = len;
    EFI_STATUS st = f->Write(f, &n, (void *)data);
    f->Flush(f);
    f->Close(f);
    return !EFI_ERROR(st) && n == len;
}

/* ---------------------------------------------------------------- machine identity */
typedef struct { char sys_vendor[64], sys_product[64], board_vendor[64], board_product[64], cpu_vendor[16], cpu_model[8]; } Ident;

static void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static void smbios_string(const uint8_t *s, uint8_t idx, char *out, size_t cap)
{
    out[0] = 0;
    if (!idx) return;
    const char *p = (const char *)s + s[1];
    for (uint8_t i = 1; i < idx && *p; i++) p += slen(p) + 1;
    size_t n = 0;
    while (p[n] && n + 1 < cap) { out[n] = p[n]; n++; }
    out[n] = 0;
}

static void identify(Ident *id)
{
    memset(id, 0, sizeof(*id));
    uint32_t r[4];
    cpuid(0, 0, r);
    memcpy(id->cpu_vendor, &r[1], 4); memcpy(id->cpu_vendor + 4, &r[3], 4); memcpy(id->cpu_vendor + 8, &r[2], 4);
    /* display family-model as "FF-MM" hex (e.g. 06-97 = Alder Lake-S, 06-55 = Skylake-X/Cascade Lake, 19-21 = Zen 3) */
    cpuid(1, 0, r);
    uint32_t fam = (r[0] >> 8) & 0xF, mod = (r[0] >> 4) & 0xF;
    if (fam == 0xF) fam += (r[0] >> 20) & 0xFF;
    if (fam == 0x6 || fam >= 0xF) mod |= ((r[0] >> 16) & 0xF) << 4;
    static const char H[] = "0123456789ABCDEF";
    id->cpu_model[0] = H[(fam >> 4) & 15]; id->cpu_model[1] = H[fam & 15]; id->cpu_model[2] = '-';
    id->cpu_model[3] = H[(mod >> 4) & 15]; id->cpu_model[4] = H[mod & 15]; id->cpu_model[5] = 0;

    const uint8_t *tbl = NULL; UINTN tlen = 0;
    for (UINTN i = 0; i < ST->NumberOfTableEntries; i++) {
        EFI_CONFIGURATION_TABLE *ct = &ST->ConfigurationTable[i];
        const uint8_t *ep = ct->VendorTable;
        if (!memcmp(&ct->VendorGuid, &gSmbios3Guid, sizeof(EFI_GUID)) && !memcmp(ep, "_SM3_", 5)) {
            tbl = (const uint8_t *)(UINTN) * (const uint64_t *)(ep + 0x10);
            tlen = *(const uint32_t *)(ep + 0x0C);
            break;
        }
        if (!memcmp(&ct->VendorGuid, &gSmbiosGuid, sizeof(EFI_GUID)) && !memcmp(ep, "_SM_", 4) && !tbl) {
            tbl = (const uint8_t *)(UINTN) * (const uint32_t *)(ep + 0x18);
            tlen = *(const uint16_t *)(ep + 0x16);
        }
    }
    if (!tbl) return;
    const uint8_t *p = tbl, *end = tbl + tlen;
    while (p + 4 <= end && p[0] != 127) {
        if (p[0] == 1 && p[1] >= 6) {
            smbios_string(p, p[4], id->sys_vendor, sizeof(id->sys_vendor));
            smbios_string(p, p[5], id->sys_product, sizeof(id->sys_product));
        } else if (p[0] == 2 && p[1] >= 6) {
            smbios_string(p, p[4], id->board_vendor, sizeof(id->board_vendor));
            smbios_string(p, p[5], id->board_product, sizeof(id->board_product));
        }
        const uint8_t *q = p + p[1];
        while (q + 1 < end && (q[0] || q[1])) q++;   /* string set ends with two NULs */
        p = q + 2;
    }
}

static int physical_cores(void)
{
    uint32_t r[4];
    cpuid(0x80000000, 0, r);
    if (r[0] < 0x80000008) return 0;
    cpuid(0x80000008, 0, r);
    int threads = (int)(r[2] & 0xFF) + 1;
    int per_core = 1;
    cpuid(0x80000000, 0, r);
    if (r[0] >= 0x8000001E) { cpuid(0x8000001E, 0, r); per_core = (int)((r[1] >> 8) & 0xFF) + 1; }
    return per_core ? threads / per_core : threads;
}

/* case-insensitive substring */
static int contains(const char *hay, const char *needle, size_t nlen)
{
    size_t h = slen(hay);
    if (!nlen) return 1;
    for (size_t i = 0; i + nlen <= h; i++) {
        size_t j = 0;
        while (j < nlen && lower(hay[i + j]) == lower(needle[j])) j++;
        if (j == nlen) return 1;
    }
    return 0;
}

/* profiles.txt: "<profile> <key>=<substring>" or "<profile> *"; '#' comments; first match wins */
static int pick_profile(const Ident *id, const Buf *rules, char *out, size_t cap)
{
    const char *p = rules->p, *end = rules->p + rules->len;
    while (p < end) {
        const char *ln = p; while (p < end && *p != '\n') p++;
        const char *le = p; if (p < end) p++;
        while (le > ln && (le[-1] == '\r' || le[-1] == ' ')) le--;
        while (ln < le && *ln == ' ') ln++;
        if (ln == le || *ln == '#') continue;
        const char *ne = ln; while (ne < le && *ne != ' ') ne++;
        const char *cond = ne; while (cond < le && *cond == ' ') cond++;
        int match = 0;
        if (cond < le && *cond == '*') match = 1;
        else {
            const char *eq = cond; while (eq < le && *eq != '=') eq++;
            if (eq < le) {
                size_t kl = (size_t)(eq - cond);
                const char *val = NULL;
                if (kl == 10 && !memcmp(cond, "sys-vendor", 10)) val = id->sys_vendor;
                else if (kl == 11 && !memcmp(cond, "sys-product", 11)) val = id->sys_product;
                else if (kl == 12 && !memcmp(cond, "board-vendor", 12)) val = id->board_vendor;
                else if (kl == 13 && !memcmp(cond, "board-product", 13)) val = id->board_product;
                else if (kl == 10 && !memcmp(cond, "cpu-vendor", 10)) val = id->cpu_vendor;
                else if (kl == 9 && !memcmp(cond, "cpu-model", 9)) val = id->cpu_model;
                if (val) match = contains(val, eq + 1, (size_t)(le - eq - 1));
            }
        }
        if (match && (size_t)(ne - ln) < cap) {
            memcpy(out, ln, (size_t)(ne - ln)); out[ne - ln] = 0;
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------- Ventoy hand-off */
#pragma pack(1)
typedef struct {
    EFI_GUID guid; uint8_t chksum; uint8_t disk_guid[16]; uint64_t disk_size; uint16_t part_id, part_type;
    char img_path[384]; uint64_t img_size; uint64_t loc_addr; uint32_t loc_len; uint64_t reserved[4];
    uint8_t disk_sig[4]; uint8_t reserved2[27];
} VtoyOsParam;
#pragma pack()
_Static_assert(sizeof(VtoyOsParam) == 512, "os_param size");

/* returns physical address of the NVS copy of os_param (+ table), or 0 */
static uint64_t ventoy_handoff(void)
{
    VtoyOsParam prm; UINTN sz = sizeof(prm); uint32_t attr = 0;
    EFI_STATUS st = ST->RuntimeServices->GetVariable(L"VentoyOsParam", (EFI_GUID *)&gVentoyGuid, &attr, &sz, &prm);
    if (EFI_ERROR(st) || sz < sizeof(prm)) {
        buf_str(&gLog, "ventoy: no VentoyOsParam (not started by Ventoy)\n");
        return 0;
    }
    uint8_t sum = 0;
    for (UINTN i = 0; i < sizeof(prm); i++) sum += ((uint8_t *)&prm)[i];
    if (sum || memcmp(&prm.guid, &gVentoyGuid, sizeof(EFI_GUID))) { buf_str(&gLog, "ventoy: bad VentoyOsParam\n"); return 0; }
    buf_str(&gLog, "ventoy: image "); buf_str(&gLog, prm.img_path); buf_str(&gLog, " size ");
    buf_dec(&gLog, prm.img_size); buf_str(&gLog, " table "); buf_hex(&gLog, prm.loc_addr); buf_str(&gLog, " len ");
    buf_dec(&gLog, prm.loc_len); buf_str(&gLog, "\n");
    if (!prm.loc_addr || prm.loc_len < 44 || prm.loc_len > 16 * 1024 * 1024) { buf_str(&gLog, "ventoy: no table\n"); return 0; }

    UINTN bytes = 4096 + prm.loc_len, pages = (bytes + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS mem = 0xFFFFFFFF;  /* below 4 GB */
    if (EFI_ERROR(BS->AllocatePages(AllocateMaxAddress, EfiACPIMemoryNVS, pages, &mem))) {
        mem = 0;
        if (EFI_ERROR(BS->AllocatePages(AllocateAnyPages, EfiACPIMemoryNVS, pages, &mem))) {
            buf_str(&gLog, "ventoy: cannot allocate NVS\n"); return 0;
        }
    }
    uint8_t *dst = (uint8_t *)(UINTN)mem;
    memset(dst, 0, pages * 4096);
    memcpy(dst + 4096, (void *)(UINTN)prm.loc_addr, prm.loc_len);
    VtoyOsParam *cp = (VtoyOsParam *)dst;
    *cp = prm;
    cp->loc_addr = mem + 4096;
    cp->chksum = 0;
    uint8_t s2 = 0;
    for (UINTN i = 0; i < sizeof(*cp); i++) s2 += dst[i];
    cp->chksum = (uint8_t)(0 - s2);
    buf_str(&gLog, "ventoy: copied to NVS "); buf_hex(&gLog, mem); buf_str(&gLog, "\n");
    return mem;
}

/* ---------------------------------------------------------------- template expansion */
static void b64(Buf *o, const uint8_t *d, UINTN n)
{
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (UINTN i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        char c[4] = {T[v >> 18 & 63], T[v >> 12 & 63], i + 1 < n ? T[v >> 6 & 63] : '=', i + 2 < n ? T[v & 63] : '='};
        buf_put(o, c, 4);
    }
}
static int hexval(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

static void expand(const Buf *in, Buf *out, const char *args, int cores)
{
    const char *p = in->p, *end = in->p + in->len;
    while (p < end) {
        if (*p == '@' && end - p >= 10 && !memcmp(p, "@VTOYARGS@", 10)) { buf_str(out, args); p += 10; continue; }
        if (*p == '@' && end - p > 7 && !memcmp(p, "@CORES:", 7)) {
            const char *q = p + 7; uint8_t bytes[64]; UINTN n = 0; int part = 0; int ok = 0;
            while (q + 1 < end && n < sizeof(bytes) - 1) {
                if (*q == ':') { if (part == 0) { bytes[n++] = (uint8_t)cores; part = 1; q++; continue; } break; }
                if (*q == '@') { if (part == 1) { ok = 1; } break; }
                int h = hexval(q[0]), l = hexval(q[1]);
                if (h < 0 || l < 0) break;
                bytes[n++] = (uint8_t)(h << 4 | l); q += 2;
            }
            if (ok) { b64(out, bytes, n); p = q + 1; continue; }
        }
        buf_put(out, p, 1); p++;
    }
}

/* ---------------------------------------------------------------- chain to OpenCore */
static EFI_STATUS start_opencore(EFI_HANDLE image, EFI_LOADED_IMAGE_PROTOCOL *li)
{
    EFI_DEVICE_PATH *dev = NULL;
    if (EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, (EFI_GUID *)&gDevicePathGuid, (void **)&dev))) return EFI_NOT_FOUND;
    UINTN dl = 0;
    for (EFI_DEVICE_PATH *n = dev; !(n->Type == 0x7F && n->SubType == 0xFF); n = (EFI_DEVICE_PATH *)((uint8_t *)n + (n->Length[0] | n->Length[1] << 8)))
        dl += (UINTN)(n->Length[0] | n->Length[1] << 8);
    static const char *path = "\\EFI\\OC\\OpenCore.efi";
    UINTN pl = (slen(path) + 1) * 2, total = dl + 4 + pl + 4;
    uint8_t *dp = NULL;
    if (EFI_ERROR(BS->AllocatePool(EfiLoaderData, total, (void **)&dp))) return EFI_NOT_FOUND;
    memcpy(dp, dev, dl);
    uint8_t *f = dp + dl;
    f[0] = 4; f[1] = 4; f[2] = (uint8_t)((4 + pl) & 0xFF); f[3] = (uint8_t)((4 + pl) >> 8);
    to16(path, (CHAR16 *)(f + 4));
    uint8_t *e = f + 4 + pl; e[0] = 0x7F; e[1] = 0xFF; e[2] = 4; e[3] = 0;
    EFI_HANDLE oc = NULL;
    EFI_STATUS st = BS->LoadImage(0, image, dp, NULL, 0, &oc);
    if (EFI_ERROR(st)) return st;
    return BS->StartImage(oc, NULL, NULL);
}

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *systab)
{
    ST = systab; BS = systab->BootServices;
    EFI_LOADED_IMAGE_PROTOCOL *li = NULL;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fs = NULL;
    if (EFI_ERROR(BS->HandleProtocol(image, (EFI_GUID *)&gLoadedImageGuid, (void **)&li)) ||
        EFI_ERROR(BS->HandleProtocol(li->DeviceHandle, (EFI_GUID *)&gFsGuid, (void **)&fs)) ||
        EFI_ERROR(fs->OpenVolume(fs, &gRoot)))
        return EFI_UNSUPPORTED;

    buf_str(&gLog, "VtoyBoot 1\n");
    Ident id; identify(&id);
    buf_str(&gLog, "system: "); buf_str(&gLog, id.sys_vendor); buf_str(&gLog, " / "); buf_str(&gLog, id.sys_product);
    buf_str(&gLog, "\nboard: "); buf_str(&gLog, id.board_vendor); buf_str(&gLog, " / "); buf_str(&gLog, id.board_product);
    buf_str(&gLog, "\ncpu: "); buf_str(&gLog, id.cpu_vendor); buf_str(&gLog, " "); buf_str(&gLog, id.cpu_model);
    int cores = physical_cores();
    buf_str(&gLog, " physical cores "); buf_dec(&gLog, (uint64_t)cores); buf_str(&gLog, "\n");

    Buf rules = {0}; char prof[64] = {0};
    if (!read_file("\\EFI\\VTOY\\profiles.txt", &rules) || !pick_profile(&id, &rules, prof, sizeof(prof))) {
        buf_str(&gLog, "profile: none matched, keeping existing config.plist\n");
    } else {
        Buf args = {0};
        uint64_t prm = ventoy_handoff();
        if (prm) { buf_str(&args, "vtoyparam="); buf_hex(&args, prm); }
        int nv = file_exists("\\EFI\\VTOY\\nvidia-installed");
        char tpath[160] = "\\EFI\\VTOY\\profiles\\";
        size_t tl = slen(tpath);
        memcpy(tpath + tl, prof, slen(prof)); tl += slen(prof);
        Buf tmpl = {0};
        int got = 0;
        if (nv) {
            memcpy(tpath + tl, "-nvidia.plist", 14);
            got = read_file(tpath, &tmpl);
        }
        if (!got) {
            memcpy(tpath + tl, ".plist", 7);
            got = read_file(tpath, &tmpl);
        }
        buf_str(&gLog, "profile: "); buf_str(&gLog, prof); buf_str(&gLog, " template "); buf_str(&gLog, tpath);
        buf_str(&gLog, got ? "\n" : " MISSING\n");
        buf_str(&gLog, "args: "); buf_str(&gLog, args.p ? args.p : ""); buf_str(&gLog, "\n");
        if (got) {
            Buf cfg = {0};
            expand(&tmpl, &cfg, args.p ? args.p : "", cores);
            buf_str(&gLog, write_file("\\EFI\\OC\\config.plist", cfg.p, cfg.len) ? "config.plist written\n"
                                                                                 : "config.plist WRITE FAILED\n");
        }
    }
    write_file("\\EFI\\VTOY\\vtoyboot.log", gLog.p, gLog.len);
    EFI_STATUS st = start_opencore(image, li);
    if (systab->ConOut) systab->ConOut->OutputString(systab->ConOut, L"VtoyBoot: cannot start \\EFI\\OC\\OpenCore.efi\r\n");
    BS->Stall(5000000);
    return st;
}
