"""Cross references in cking.rpx: which strings and data each function references.

The relocation table records every instruction that forms a data address (lis/addi pairs as
ADDR16_HA/LO), so string references per function are exact, not heuristic.
"""
import bisect
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "recomp"))
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from analyze import Program  # noqa: E402
from rpx import R_PPC_ADDR16_LO, R_PPC_ADDR32  # noqa: E402


class Xref:
    def __init__(self, rpx_path):
        self.p = p = Program(rpx_path)
        self.funcs = p.discover()
        r = p.rpx
        self.ro = r.by_name[".rodata"]
        self.da = r.by_name[".data"]
        self.refs = {}  # function -> list of (site, target)
        for sec, addr, typ, sym, add in r.relocs:
            if sym.import_lib or sec.name != ".text" or typ != R_PPC_ADDR16_LO:
                continue
            tgt = (sym.value + add) & 0xFFFFFFFF
            self.refs.setdefault(self.func_of(addr), []).append((addr, tgt))
        for v in self.refs.values():
            v.sort()

    def func_of(self, addr):
        i = bisect.bisect_right(self.funcs, addr) - 1
        return self.funcs[i] if i >= 0 else None

    def cstr(self, addr):
        for s in (self.ro, self.da):
            if s.addr <= addr < s.addr + s.size:
                d = s.data
                o = addr - s.addr
                e = d.find(b"\0", o)
                raw = d[o:e if e >= 0 else o + 200]
                if len(raw) >= 2 and all(0x20 <= b < 0x7F or b in (9, 10) for b in raw):
                    return raw.decode()
                # Shift-JIS text (debug messages) is common too
                if len(raw) >= 2:
                    try:
                        return raw.decode("shift_jis")
                    except UnicodeDecodeError:
                        return None
        return None

    def floats(self, func):
        """float constants loaded by func (lfs/lfd from a relocated address)"""
        import struct
        out = []
        for site, tgt in self.refs.get(func, []):
            w = self.p.word(site & ~3)
            op = w >> 26
            if op not in (48, 50):  # lfs, lfd
                continue
            for s in (self.ro, self.da):
                if s.addr <= tgt < s.addr + s.size - 8:
                    o = tgt - s.addr
                    v = struct.unpack_from(">f" if op == 48 else ">d", s.data, o)[0]
                    out.append(v)
        return out

    def strings(self, func):
        out = []
        for site, tgt in self.refs.get(func, []):
            s = self.cstr(tgt)
            if s is not None:
                out.append((site, tgt, s))
        return out


if __name__ == "__main__":
    x = Xref(sys.argv[1])
    n = sum(1 for f in x.funcs if x.strings(f))
    print("functions", len(x.funcs), "with string refs", n)
    for f in x.funcs:
        ss = x.strings(f)
        if any(s.endswith(".cpp") for _, _, s in ss):
            print("%08X" % f, [s for _, _, s in ss][:8])
            break
