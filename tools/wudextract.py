#!/usr/bin/env python3
"""Wii U disc image (.wud/.wux) reader and extractor.

Port of Cemu's src/Cafe/Filesystem/WUD/wud.cpp and FST/FST.cpp
(Copyright (c) Cemu contributors, licensed under the Mozilla Public License 2.0,
see runtime/third_party/cemu/LICENSE.txt).

usage:
  wudextract.py IMAGE list [GLOB]
  wudextract.py IMAGE extract OUTDIR [GLOB ...]

Keys (none are included in this repository; dump them from your own console):
  - the disc key is read from IMAGE with the extension replaced by .key (16 bytes);
  - the Wii U common key is read from the WIIU_COMMON_KEY environment variable (32 hex digits),
    or from a file common.key (16 raw bytes or 32 hex digits) next to IMAGE or in the current
    directory.
"""
import fnmatch
import hashlib
import os
import struct
import sys

from Crypto.Cipher import AES

SECTOR = 0x8000
BLOCK_SIZE = 0x10000
BLOCK_HASH_SIZE = 0x400
BLOCK_FILE_SIZE = 0xFC00


def aes_dec(key, iv, data):
    return AES.new(key, AES.MODE_CBC, iv).decrypt(data)


def parse_key(data, what):
    data = data.strip() if len(data) != 16 else data
    if len(data) == 32:
        try:
            data = bytes.fromhex(data.decode("ascii"))
        except ValueError:
            pass
    if len(data) != 16:
        sys.exit("%s: expected 16 bytes or 32 hex digits" % what)
    return data


def common_key(image):
    """The Wii U common key, supplied by the user (it is not part of this repository)."""
    env = os.environ.get("WIIU_COMMON_KEY")
    if env:
        return parse_key(env.encode(), "WIIU_COMMON_KEY")
    for d in (os.path.dirname(os.path.abspath(image)), os.getcwd()):
        p = os.path.join(d, "common.key")
        if os.path.isfile(p):
            return parse_key(open(p, "rb").read(), p)
    sys.exit("Wii U common key not found: set WIIU_COMMON_KEY or put common.key next to the image "
             "(dump it from your own console)")


class Wud:
    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.f.read(32)
        magic0, magic1, sector_size = struct.unpack_from("<III", hdr, 0)
        if magic0 == 0x30585557 and magic1 == 0x1099D02E:  # "WUX0"
            self.compressed = True
            self.sector_size = sector_size
            self.size, = struct.unpack_from("<Q", hdr, 16)
            n = (self.size + sector_size - 1) // sector_size
            self.index = struct.unpack("<%dI" % n, self.f.read(4 * n))
            off = 32 + 4 * n
            self.sector_base = (off + sector_size - 1) // sector_size * sector_size
        else:
            self.compressed = False
            self.size = os.fstat(self.f.fileno()).st_size

    def read(self, offset, length):
        if not self.compressed:
            self.f.seek(offset)
            return self.f.read(length)
        out = bytearray()
        while length > 0:
            sec_off = offset % self.sector_size
            n = min(self.sector_size - sec_off, length)
            real = self.index[offset // self.sector_size]
            self.f.seek(self.sector_base + real * self.sector_size + sec_off)
            out += self.f.read(n)
            offset += n
            length -= n
        return bytes(out)


class Entry:
    __slots__ = ("name", "path", "is_dir", "offset", "size", "cluster", "flags")


class FST:
    def __init__(self, wud, base, fst_offset, fst_size, key):
        self.wud, self.base, self.key = wud, base, key
        padded = (fst_size + 15) & ~15
        data = aes_dec(key, bytes(16), wud.read(base + fst_offset, padded))[:fst_size]
        magic, self.offset_factor, ncluster = struct.unpack_from(">III", data, 0)
        if magic != 0x46535400:
            raise ValueError("bad FST magic (wrong key?)")
        self.clusters = []
        for i in range(ncluster):
            off, size = struct.unpack_from(">II", data, 0x20 + i * 0x20)
            hash_mode = data[0x20 + i * 0x20 + 0x14]
            self.clusters.append((off, size, hash_mode))
        ft = 0x20 + ncluster * 0x20
        nentries, = struct.unpack_from(">I", data, ft + 8)
        names = data[ft + nentries * 0x10:]

        def name_at(o):
            return names[o:names.index(b"\0", o)].decode("utf-8", "replace")

        self.entries = []
        stack = [("", nentries)]  # (dir path, end index)
        for i in range(nentries):
            while i >= stack[-1][1]:
                stack.pop()
            tno, off, size, _perm, cl = struct.unpack_from(">IIIHH", data, ft + i * 0x10)
            e = Entry()
            e.flags = tno >> 24
            e.name = name_at(tno & 0xFFFFFF) if i else ""
            e.path = (stack[-1][0] + "/" + e.name).lstrip("/")
            e.is_dir = bool(e.flags & 1)
            e.offset, e.size, e.cluster = off, size, cl
            self.entries.append(e)
            if e.is_dir and i:
                stack.append((e.path, size))
        self._raw_cache = {}

    def _cluster_base(self, cl):
        return self.base + self.clusters[cl][0] * SECTOR

    def read_file(self, e, out):
        mode = self.clusters[e.cluster][2]
        pos = e.offset * self.offset_factor
        remaining = e.size
        base = self._cluster_base(e.cluster)
        if mode == 2:  # hashed/interleaved
            blk, within = divmod(pos, BLOCK_FILE_SIZE)
            while remaining > 0:
                raw = self.wud.read(base + blk * BLOCK_SIZE, BLOCK_SIZE)
                hashes = aes_dec(self.key, bytes(16), raw[:BLOCK_HASH_SIZE])
                h0 = hashes[(blk % 16) * 20:(blk % 16) * 20 + 16]
                fdata = aes_dec(self.key, h0, raw[BLOCK_HASH_SIZE:])
                if hashlib.sha1(fdata).digest() != hashes[(blk % 16) * 20:(blk % 16) * 20 + 20]:
                    raise IOError("H0 hash mismatch in %s block %d" % (e.path, blk))
                n = min(remaining, BLOCK_FILE_SIZE - within)
                out.write(fdata[within:within + n])
                remaining -= n
                within = 0
                blk += 1
        else:  # raw: CBC over whole cluster, IV = cluster index for block 0
            blk, within = divmod(pos, SECTOR)
            if blk == 0:
                iv = bytes([(e.cluster >> 8) & 0xFF, e.cluster & 0xFF]) + bytes(14)
            else:
                iv = self.wud.read(base + blk * SECTOR - 16, 16)
            # decrypt in larger chunks for speed
            chunk_sectors = 64
            while remaining > 0:
                raw = self.wud.read(base + blk * SECTOR, SECTOR * chunk_sectors)
                dec = aes_dec(self.key, iv, raw)
                iv = raw[-16:]
                n = min(remaining, len(dec) - within)
                out.write(dec[within:within + n])
                remaining -= n
                within = 0
                blk += chunk_sectors


def open_disc(path):
    key = open(os.path.splitext(path)[0] + ".key", "rb").read(16)
    wud = Wud(path)
    if struct.unpack(">I", wud.read(SECTOR * 2, 4))[0] != 0xCC549EB9:
        raise ValueError("not a Wii U disc image")
    pt = aes_dec(key, bytes(16), wud.read(SECTOR * 3, SECTOR))
    magic, bsize, = struct.unpack_from(">II", pt, 0)
    if magic != 0xCCA6E67B:
        raise ValueError("partition table decryption failed (wrong disc key)")
    nparts, = struct.unpack_from(">I", pt, 0x1C)
    parts = []
    for i in range(nparts):
        ent = pt[0x800 + i * 0x80:0x800 + (i + 1) * 0x80]
        name = ent[:31].split(b"\0")[0].decode()
        addr, = struct.unpack_from(">I", ent, 0x20)
        parts.append((name, addr * SECTOR))

    def part_fst(base, key):
        ph = wud.read(base, 0x60)
        fst_size, fst_sector = struct.unpack_from(">II", ph, 0x14)
        return FST(wud, base, fst_sector * SECTOR, fst_size, key)

    si_idx = next(i for i, p in enumerate(parts) if p[0].startswith("SI"))
    gm_idx = next(i for i, p in enumerate(parts) if p[0].startswith("GM"))
    si = part_fst(parts[si_idx][1], key)
    tik_entry = next(e for e in si.entries if e.path == "%02x/title.tik" % gm_idx)

    class Buf:
        def __init__(self):
            self.b = bytearray()

        def write(self, d):
            self.b += d

    buf = Buf()
    si.read_file(tik_entry, buf)
    tik = bytes(buf.b)
    enc_title_key = tik[0x1BF:0x1CF]
    title_id = tik[0x1DC:0x1E4]
    title_key = aes_dec(common_key(path), title_id + bytes(8), enc_title_key)
    gm = part_fst(parts[gm_idx][1], title_key)
    return parts, gm, title_id.hex()


def main():
    image, cmd = sys.argv[1], sys.argv[2]
    parts, gm, tid = open_disc(image)
    if cmd == "list":
        pat = sys.argv[3] if len(sys.argv) > 3 else "*"
        print("title id", tid, "partitions", [p[0] for p in parts], file=sys.stderr)
        for e in gm.entries:
            if not e.is_dir and fnmatch.fnmatch(e.path, pat):
                print("%10d  %s" % (e.size, e.path))
    elif cmd == "extract":
        outdir = sys.argv[3]
        pats = sys.argv[4:] or ["*"]
        for e in gm.entries:
            if e.is_dir or e.flags & 0x80 or not any(fnmatch.fnmatch(e.path, p) for p in pats):
                continue
            dst = os.path.join(outdir, e.path)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            with open(dst, "wb") as out:
                gm.read_file(e, out)
            print(e.path, file=sys.stderr)


if __name__ == "__main__":
    main()
