"""FAT32 builder (pure python, no admin rights, long file names).

  make_fat_img.py <srcdir> <out.img> <size_mib> [label]
      New GPT disk image with a single FAT32 EFI System Partition populated from <srcdir>.

  make_fat_img.py --into <disk.raw> <srcdir> [label]
      Re-format GPT partition 1 (the ESP) of an existing raw disk in place and populate it
      from <srcdir>. Nothing outside that partition is touched. Cluster size is chosen so
      the volume stays FAT32 (macOS' 200 MiB ESP needs 512-byte clusters).
"""
import os, struct, sys, uuid, zlib

SEC = 512
RSVD = 32
PART_START = 2048
ESP_TYPE = uuid.UUID('C12A7328-F81F-11D2-BA4B-00A0C93EC93B')
DATE = ((2026 - 1980) << 9) | (11 << 5) | 11
TIME = (12 << 11)


class Node:
    def __init__(self, name, path, is_dir):
        self.name, self.path, self.is_dir = name, path, is_dir
        self.children, self.cluster, self.size = [], 0, 0


def scan(path, name=''):
    n = Node(name, path, True)
    for e in sorted(os.listdir(path), key=str.lower):
        p = os.path.join(path, e)
        if os.path.isdir(p):
            n.children.append(scan(p, e))
        else:
            c = Node(e, p, False)
            c.size = os.path.getsize(p)
            n.children.append(c)
    return n


def is_short(name):
    if name in ('.', '..'):
        return True
    base, _, ext = name.partition('.')
    ok = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-~$')
    return (name.upper() == name and 0 < len(base) <= 8 and len(ext) <= 3 and '.' not in ext
            and set(base) <= ok and set(ext) <= ok)


def short_name(name, idx):
    if is_short(name):
        base, _, ext = name.partition('.')
        return base.ljust(8).encode() + ext.ljust(3).encode()
    ext = ''.join(ch for ch in name.rpartition('.')[2].upper() if ch.isalnum())[:3] if '.' in name else ''
    base = ''.join(ch for ch in name.upper() if ch.isalnum())[:4] or 'F'
    return f'{base}~{idx:03X}'[:8].ljust(8).encode() + ext.ljust(3).encode()


def lfn_entries(name, sn):
    csum = 0
    for b in sn:
        csum = (((csum & 1) << 7) + (csum >> 1) + b) & 0xFF
    u = name.encode('utf-16-le')
    chars = [u[i:i + 2] for i in range(0, len(u), 2)]
    if len(chars) % 13:
        chars.append(b'\x00\x00')
        while len(chars) % 13:
            chars.append(b'\xff\xff')
    parts = [chars[i:i + 13] for i in range(0, len(chars), 13)]
    out = []
    for i, p in reversed(list(enumerate(parts, 1))):
        o = i | (0x40 if i == len(parts) else 0)
        e = bytes([o]) + b''.join(p[0:5]) + bytes([0x0F, 0, csum]) + b''.join(p[5:11]) + b'\x00\x00' + b''.join(p[11:13])
        out.append(e)
    return out


def dirent(sn, attr, cluster, size):
    return struct.pack('<11sBBBHHHHHHHI', sn, attr, 0, 0, TIME, DATE, DATE, cluster >> 16, TIME, DATE, cluster & 0xFFFF, size)


def fat_geometry(part_secs, spc):
    fatsz = 1
    while True:
        clusters = (part_secs - RSVD - 2 * fatsz) // spc
        need = ((clusters + 2) * 4 + SEC - 1) // SEC
        if need <= fatsz:
            return fatsz, clusters
        fatsz = need


def write_fat32(f, part_lba, part_secs, src, label='EFI', spc=None):
    """Format a FAT32 volume at sector part_lba (part_secs long) inside open file f, fill it from src."""
    if spc is None:  # largest cluster (<=4 KiB) that still gives >= 65525 clusters
        spc = next(s for s in (8, 4, 2, 1) if fat_geometry(part_secs, s)[1] >= 65525)
    cl = SEC * spc
    fatsz, clusters = fat_geometry(part_secs, spc)
    assert clusters >= 65525, 'partition too small for FAT32'
    data_start = RSVD + 2 * fatsz

    root = scan(src)
    fat = [0x0FFFFFF8, 0x0FFFFFFF]
    nxt = [2]

    def alloc(nbytes):
        n = max(1, (nbytes + cl - 1) // cl)
        first = nxt[0]
        for i in range(n):
            fat.append(first + i + 1 if i < n - 1 else 0x0FFFFFFF)
        nxt[0] += n
        assert nxt[0] < clusters + 2, 'volume full'
        return first

    counter = [1]

    def dir_bytes(node, parent_cluster, is_root):
        ents = []
        if not is_root:
            ents.append(dirent(b'.          ', 0x10, node.cluster, 0))
            ents.append(dirent(b'..         ', 0x10, parent_cluster, 0))
        else:
            ents.append(struct.pack('<11sB20s', label.upper()[:11].ljust(11).encode(), 0x08, b'\x00' * 20))
        for c in node.children:
            sn = short_name(c.name, counter[0]); counter[0] += 1
            if not is_short(c.name):
                ents.extend(lfn_entries(c.name, sn))
            ents.append(dirent(sn, 0x10 if c.is_dir else 0x20, c.cluster, 0 if c.is_dir else c.size))
        return b''.join(ents)

    def count_entries(node, is_root):
        n = 1 if is_root else 2
        for c in node.children:
            n += 1 + (0 if is_short(c.name) else (len(c.name.encode('utf-16-le')) // 2 + 12) // 13)
        return n * 32

    def walk(node, is_root=False):
        node.cluster = alloc(count_entries(node, is_root) + 32)
        for c in node.children:
            if c.is_dir:
                walk(c)
            elif c.size:
                c.cluster = alloc(c.size)
    walk(root, True)

    part_off = part_lba * SEC
    data_off = part_off + data_start * SEC

    def coff(c):
        return data_off + (c - 2) * cl

    # zero reserved area + both FATs + the clusters we use (old contents must not leak through)
    zero = bytes(1 << 20)
    used_end = coff(nxt[0])
    pos = part_off
    while pos < used_end:
        n = min(len(zero), used_end - pos)
        f.seek(pos); f.write(zero[:n]); pos += n

    def write_tree(node, parent_cluster, is_root):
        data = dir_bytes(node, 0 if parent_cluster == 2 else parent_cluster, is_root)
        f.seek(coff(node.cluster)); f.write(data)
        for c in node.children:
            if c.is_dir:
                write_tree(c, node.cluster, False)
            elif c.size:
                f.seek(coff(c.cluster))
                with open(c.path, 'rb') as s:
                    while chunk := s.read(16 << 20):
                        f.write(chunk)
    write_tree(root, 0, True)

    fat_bytes = struct.pack(f'<{len(fat)}I', *fat)
    for i in range(2):
        f.seek(part_off + (RSVD + i * fatsz) * SEC)
        f.write(fat_bytes)

    bs = bytearray(SEC)
    bs[0:3] = b'\xEB\x58\x90'
    bs[3:11] = b'MSWIN4.1'
    struct.pack_into('<HBHBHHBHHHII', bs, 11, SEC, spc, RSVD, 2, 0, 0, 0xF8, 0, 63, 255, part_lba, part_secs)
    struct.pack_into('<IHHIHH', bs, 36, fatsz, 0, 0, 2, 1, 6)
    struct.pack_into('<BBBI11s8s', bs, 64, 0x80, 0, 0x29, uuid.uuid4().int & 0xFFFFFFFF,
                     label.upper()[:11].ljust(11).encode(), b'FAT32   ')
    bs[510:512] = b'\x55\xAA'
    fsi = bytearray(SEC)
    struct.pack_into('<I', fsi, 0, 0x41615252)
    struct.pack_into('<III', fsi, 484, 0x61417272, clusters - (nxt[0] - 2), nxt[0])
    struct.pack_into('<I', fsi, 508, 0xAA550000)
    for base in (0, 6):
        f.seek(part_off + base * SEC); f.write(bs)
        f.seek(part_off + (base + 1) * SEC); f.write(fsi)
    return clusters, nxt[0] - 2, spc


def build(src, out, size_mib, label='EFI'):
    total = size_mib * 1024 * 1024 // SEC
    part_secs = total - PART_START - 34
    part_secs -= part_secs % 8
    with open(out, 'wb') as f:
        f.truncate(total * SEC)
        clusters, used, _ = write_fat32(f, PART_START, part_secs, src, label, spc=8)

        disk_guid = uuid.uuid4()
        entry = struct.pack('<16s16sQQQ72s', ESP_TYPE.bytes_le, uuid.uuid4().bytes_le, PART_START,
                            PART_START + part_secs - 1, 0, 'EFI System Partition'.encode('utf-16-le').ljust(72, b'\0'))
        entries = entry.ljust(128 * 128, b'\0')
        ecrc = zlib.crc32(entries)

        def header(cur, backup, entries_lba):
            h = bytearray(struct.pack('<8sIIIIQQQQ16sQIII', b'EFI PART', 0x00010000, 92, 0, 0, cur, backup,
                                      34, total - 34, disk_guid.bytes_le, entries_lba, 128, 128, ecrc))
            struct.pack_into('<I', h, 16, zlib.crc32(bytes(h)))
            return bytes(h).ljust(SEC, b'\0')

        mbr = bytearray(SEC)
        struct.pack_into('<B3sB3sII', mbr, 446, 0, b'\x00\x02\x00', 0xEE, b'\xff\xff\xff', 1, min(total - 1, 0xFFFFFFFF))
        mbr[510:512] = b'\x55\xAA'
        f.seek(0); f.write(mbr)
        f.seek(SEC); f.write(header(1, total - 1, 2))
        f.seek(2 * SEC); f.write(entries)
        f.seek((total - 33) * SEC); f.write(entries)
        f.seek((total - 1) * SEC); f.write(header(total - 1, 1, total - 33))
    print(f'{out}: {size_mib} MiB, FAT32 {clusters} clusters, used {used}')


def into(disk, src, label='EFI'):
    with open(disk, 'r+b') as f:
        f.seek(SEC)
        h = f.read(SEC)
        assert h[:8] == b'EFI PART', 'no GPT header at LBA 1'
        entries_lba, = struct.unpack_from('<Q', h, 72)
        f.seek(entries_lba * SEC)
        e = f.read(128)
        assert uuid.UUID(bytes_le=e[:16]) == ESP_TYPE, 'partition 1 is not an EFI System Partition'
        first, last = struct.unpack_from('<QQ', e, 32)
        clusters, used, spc = write_fat32(f, first, last - first + 1, src, label)
    print(f'{disk}: ESP LBA {first}-{last} reformatted FAT32, {spc * SEC} B clusters, {clusters} clusters, used {used}')


if __name__ == '__main__':
    if sys.argv[1] == '--into':
        into(sys.argv[2], sys.argv[3], *(sys.argv[4:5]))
    else:
        build(sys.argv[1], sys.argv[2], int(sys.argv[3]), *(sys.argv[4:5]))
