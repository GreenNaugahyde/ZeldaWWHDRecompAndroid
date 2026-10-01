#!/usr/bin/env python3
"""Disassemble WWHD functions with names: wdis.py ADDR [ADDR...]  (whole functions, bl/b targets
named from build/names.tsv, float constants from .rodata shown)"""
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, HERE)
from ppcdis import dis  # noqa: E402
from rpx import Rpx  # noqa: E402
from funcdb import GenIndex, load_names  # noqa: E402

gen = GenIndex(os.path.join(ROOT, "build", "gen"))
names, gc = load_names(os.path.join(ROOT, "build"))
rpx = Rpx(os.path.join(ROOT, "game", "code", "cking.rpx"))
secs = [s for s in rpx.sections if s.name in (".rodata", ".data") and s.data]


def word(a):
    for s in secs:
        if s.addr <= a < s.addr + len(s.data) - 3:
            return struct.unpack(">I", s.data[a - s.addr:a - s.addr + 4])[0]
    return None


for arg in sys.argv[1:]:
    a = int(arg, 16)
    n = gen.size(a) // 4
    print("== %08X %s (%d insns)" % (a, names.get(a, ("?",))[0], n))
    hi = {}
    for line in dis(a, n).split("\n"):
        m = re.match(r"([0-9a-f]{8}): ([0-9a-f]{8})\s+(\S+)\s*(.*)", line)
        if not m:
            print(line)
            continue
        ad, raw, mn, ops = m.groups()
        note = ""
        t = re.search(r"0x([0-9a-f]+)$", ops)
        if mn in ("bl", "b") and t:
            ta = int(t.group(1), 16)
            note = names.get(ta, ("",))[0] or ("f_%08X" % ta)
        mm = re.match(r"(r\d+), (-?0x[0-9a-f]+|\d+)$", ops)
        if mn == "lis" and mm:
            hi[mm.group(1)] = int(mm.group(2), 16) << 16
        ml = re.match(r"(f\d+|r\d+), (-?0x[0-9a-f]+|-?\d+)\((r\d+)\)$", ops)
        if ml and ml.group(3) in hi:
            ea = (hi[ml.group(3)] + int(ml.group(2), 0)) & 0xFFFFFFFF
            w = word(ea)
            if mn in ("lfs",) and w is not None:
                note = "[%08X] = %r" % (ea, struct.unpack(">f", struct.pack(">I", w))[0])
            else:
                note = "[%08X]" % ea
        ma = re.match(r"(r\d+), (r\d+), (-?0x[0-9a-f]+|-?\d+)$", ops)
        if mn == "addi" and ma and ma.group(2) in hi:
            note = "= %08X" % ((hi[ma.group(2)] + int(ma.group(3), 0)) & 0xFFFFFFFF)
        print("%s  %-8s %-28s %s" % (ad, mn, ops, note))
