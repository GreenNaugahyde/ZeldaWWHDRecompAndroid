#!/usr/bin/env python3
"""Dump the RPX's loaded sections (.text, .rodata, .data, ...) for the harness: generated inputs
read constants, tables and vtables from the real image. Output is game data: keep it in build/.

usage: mkimage.py game/code/cking.rpx build/verify/image.bin
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
from rpx import Rpx

r = Rpx(sys.argv[1])
os.makedirs(os.path.dirname(sys.argv[2]), exist_ok=True)
with open(sys.argv[2], "wb") as f:
    for s in r.sections:
        if s.name in (".syscall", ".text", ".rodata", ".data", ".module_id") and s.data:
            f.write(struct.pack("<II", s.addr, len(s.data)))
            f.write(s.data)
            print("%-12s %08X %8X" % (s.name, s.addr, len(s.data)))
