#!/usr/bin/env python3
"""Disassemble cking.rpx .text: ppcdis.py ADDR [COUNT]"""
import sys, os
sys.path.insert(0, os.path.dirname(__file__))
from rpx import Rpx
import capstone
_rpx = None
def dis(addr, count=32, path=os.path.join(os.path.dirname(__file__), "..", "game/code/cking.rpx")):
    global _rpx
    _rpx = _rpx or Rpx(path)
    t = _rpx.by_name[".text"]
    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN | capstone.CS_MODE_PS)
    o = addr - t.addr
    out = []
    for k in range(count):
        chunk = t.data[o + 4*k:o + 4*k + 4]
        ins = next(md.disasm(chunk, addr + 4*k), None)
        out.append("%08x: %s  %s" % (addr + 4*k, chunk.hex(), "%s %s" % (ins.mnemonic, ins.op_str) if ins else "??"))
    return "\n".join(out)
if __name__ == "__main__":
    print(dis(int(sys.argv[1], 16), int(sys.argv[2]) if len(sys.argv) > 2 else 32))
