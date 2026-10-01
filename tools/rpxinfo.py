#!/usr/bin/env python3
"""Dump sections and imported functions of a Wii U RPX."""
import struct, sys, zlib, collections
d = open(sys.argv[1], "rb").read()
shoff, = struct.unpack_from(">I", d, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from(">HHH", d, 0x2E)
secs = []
for i in range(shnum):
    name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(">10I", d, shoff + i * shentsize)
    raw = d[off:off + size] if typ != 8 else b""
    if flags & 0x08000000 and raw:  # SHF_RPL_ZLIB
        raw = zlib.decompress(raw[4:])
    secs.append(dict(name=name, type=typ, flags=flags, addr=addr, size=len(raw) if raw else size, raw=raw, link=link))
strtab = secs[shstrndx]["raw"]
nm = lambda o, t: t[o:t.index(b"\0", o)].decode()
for s in secs:
    s["n"] = nm(s["name"], strtab)
if "-s" in sys.argv:
    for s in secs:
        print("%-24s type=%08x addr=%08x size=%8x flags=%08x" % (s["n"], s["type"], s["addr"], s["size"], s["flags"]))
# imports: symbols whose section is a .fimport_/.dimport_ section
imports = collections.defaultdict(set)
for s in secs:
    if s["type"] != 2:  # SHT_SYMTAB
        continue
    st = secs[s["link"]]["raw"]
    for j in range(len(s["raw"]) // 16):
        name, value, size, info, other, shndx = struct.unpack_from(">IIIBBH", s["raw"], j * 16)
        if 0 < shndx < len(secs) and secs[shndx]["n"].startswith((".fimport_", ".dimport_")):
            lib = secs[shndx]["n"].split("_", 1)[1]
            kind = "f" if secs[shndx]["n"][1] == "f" else "d"
            imports[lib].add((kind, nm(name, st)))
tot = 0
for lib in sorted(imports):
    f = sorted(n for k, n in imports[lib])
    tot += len(f)
    print("%-12s %4d  %s" % (lib, len(f), " ".join(f) if "-v" in sys.argv else ""))
print("total imports:", tot)
