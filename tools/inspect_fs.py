#!/usr/bin/env python3
"""Inspect and verify an MVFS image: checks CRCs, bitmaps, block ownership, and lists / extracts files.

Usage:
    python3 tools/inspect_fs.py image.img            # verify + list
    python3 tools/inspect_fs.py image.img --extract name out_path
"""
import struct
import sys
import zlib

BS = 4096
INODE_SIZE = 128
DIRENT_SIZE = 64


def bit(bitmap, i):
    return (bitmap[i // 8] >> (i % 8)) & 1


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    d = open(sys.argv[1], "rb").read()
    errors = []

    sb = struct.unpack_from("<IIIQQQQQQQQQQQQII", d, 0)
    (magic, version, bsize, total, icount, ibm_s, ibm_n, dbm_s, dbm_n,
     it_s, it_n, dr_s, dr_n, root, mtime, flags, crc) = sb
    if magic != 0x4D565346:
        print("bad magic")
        return 1
    blk = bytearray(d[:BS])
    blk[112:116] = b"\0" * 4
    if zlib.crc32(bytes(blk[:BS - 4])) != crc:
        errors.append("superblock CRC mismatch")
    if len(d) != total * BS:
        errors.append(f"image size {len(d)} != total_blocks*BS {total * BS}")

    ibm = d[ibm_s * BS:(ibm_s + 1) * BS]
    dbm = d[dbm_s * BS:(dbm_s + 1) * BS]

    def read_inode(n):
        o = it_s * BS + (n - 1) * INODE_SIZE
        raw = bytearray(d[o:o + INODE_SIZE])
        mode, links, uid, gid, size = struct.unpack_from("<HHIIQ", raw, 0)
        direct = struct.unpack_from("<12I", raw, 44)
        stored = struct.unpack_from("<Q", raw, 120)[0]
        raw[120:128] = b"\0" * 8
        ok = zlib.crc32(bytes(raw[:120])) == stored
        return mode, links, size, direct, ok

    used_blocks = {}
    files = []
    rmode, rlinks, rsize, rdirect, rok = read_inode(root)
    if not rok:
        errors.append("root inode CRC mismatch")
    root_blk = rdirect[0]
    used_blocks[root_blk] = "root dir"
    entries = 0
    for i in range(BS // DIRENT_SIZE):
        e = d[root_blk * BS + i * DIRENT_SIZE: root_blk * BS + (i + 1) * DIRENT_SIZE]
        ino, typ = struct.unpack_from("<IB", e, 0)
        if ino == 0:
            continue
        entries += 1
        x = 0
        for b in e[:63]:
            x ^= b
        if x != e[63]:
            errors.append(f"dirent {i} checksum mismatch")
        name = e[5:63].split(b"\0")[0].decode(errors="replace")
        if not bit(ibm, ino - 1):
            errors.append(f"{name}: inode {ino} not marked in inode bitmap")
        if name in (".", ".."):
            continue
        mode, links, size, direct, ok = read_inode(ino)
        if not ok:
            errors.append(f"{name}: inode CRC mismatch")
        need = (size + BS - 1) // BS
        blocks = [b for b in direct if b]
        if len(blocks) != need:
            errors.append(f"{name}: {len(blocks)} blocks but size needs {need}")
        for b in blocks:
            if b in used_blocks:
                errors.append(f"{name}: block {b} also used by {used_blocks[b]}")
            used_blocks[b] = name
            if not (dr_s <= b < dr_s + dr_n):
                errors.append(f"{name}: block {b} outside data region")
            elif not bit(dbm, b - dr_s):
                errors.append(f"{name}: block {b} not marked in data bitmap")
        files.append((name, ino, size, blocks))

    # root size must match entry count
    if rsize != entries * DIRENT_SIZE:
        errors.append(f"root size {rsize} != entries*64 ({entries * DIRENT_SIZE})")
    # bitmap bits set must equal blocks actually owned
    set_data = sum(bit(dbm, i) for i in range(dr_n))
    if set_data != len(used_blocks):
        errors.append(f"data bitmap has {set_data} bits set but {len(used_blocks)} blocks are owned")

    if len(sys.argv) >= 5 and sys.argv[2] == "--extract":
        for name, ino, size, blocks in files:
            if name == sys.argv[3]:
                data = b"".join(d[b * BS:(b + 1) * BS] for b in blocks)[:size]
                open(sys.argv[4], "wb").write(data)
                print(f"extracted {name} ({size} bytes)")
                break
        else:
            print("file not found")
            return 1

    print(f"MVFS image: {total} blocks, {icount} inodes, data region {dr_s}..{dr_s + dr_n - 1}")
    for name, ino, size, blocks in files:
        print(f"  {name:<30} inode {ino:<4} {size:>6} bytes  blocks {blocks}")
    if errors:
        print("ERRORS:")
        for e in errors:
            print("  -", e)
        return 1
    print("OK: all checksums, bitmaps and block ownership are consistent")
    return 0


if __name__ == "__main__":
    sys.exit(main())
