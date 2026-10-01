"""Actor profiles: WWHD's g_profile_* structures matched to the decompilation's by process name.

Each actor registers {..., proc name (s16 at +0x08), ..., actor method table (+0x24)}; the method
table holds Create, Delete, Execute, IsDelete, Draw. Same layout in WWHD, so the process name links
a WWHD profile to the decompiled actor, and its five entries name five functions.
"""
import os
import re
import struct

from xref import Xref  # noqa: F401  (sets up the tools path)
from rpx import R_PPC_ADDR32

METHODS = ("Create", "Delete", "Execute", "IsDelete", "Draw")


def proc_names(tww):
    out = {}
    for line in open(os.path.join(tww, "include", "f_pc", "f_pc_name.h")):
        m = re.search(r"/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*(fpcNm_\w+_e)", line)
        if m:
            out[m.group(2)] = int(m.group(1), 16)
    return out


def decomp_profiles(tww):
    """proc name value -> (file, [5 method function names])"""
    names = proc_names(tww)
    out = {}
    for d, _, files in os.walk(os.path.join(tww, "src")):
        for f in files:
            if not f.endswith(".cpp"):
                continue
            src = open(os.path.join(d, f), encoding="utf-8", errors="replace").read()
            tables = {}
            for m in re.finditer(r"actor_method_class\s+(\w+)\s*=\s*\{(.*?)\};", src, re.S):
                fns = re.findall(r"\(process_method_func\)\s*([\w:]+)", m.group(2))
                if len(fns) == 5:
                    tables[m.group(1)] = fns
            for m in re.finditer(r"actor_process_profile_definition2?\s+g_profile_\w+\s*=\s*\{(.*?)\};", src, re.S):
                body = m.group(1)
                pn = re.search(r"(fpcNm_\w+_e)", body)
                tb = re.search(r"&(\w+)\s*,\s*/\*\s*Status|Actor SubMtd\s*\*/\s*&(\w+)", body)
                tname = tb and (tb.group(1) or tb.group(2))
                if pn and pn.group(1) in names and tname in tables:
                    out[names[pn.group(1)]] = (f, tables[tname])
    return out


def wwhd_profiles(x):
    """list of (profile address, proc name, [5 function addresses])"""
    r = x.p.rpx
    ptr = {}
    for sec, addr, typ, sym, add in r.relocs:
        if not sym.import_lib and typ == R_PPC_ADDR32 and sec.name in (".data", ".rodata"):
            ptr[addr] = (sym.value + add) & 0xFFFFFFFF
    fset = set(x.funcs)

    def rd16(a):
        for s in (r.by_name[".data"], r.by_name[".rodata"]):
            if s.addr <= a < s.addr + s.size:
                return struct.unpack_from(">h", s.data, a - s.addr)[0]

    out = []
    for site, t in ptr.items():
        tbl = [ptr.get(t + 4 * i) for i in range(5)]
        base = site - 0x24
        if all(v in fset for v in tbl) and base + 0xC in ptr and base + 0x1C in ptr:
            out.append((base, rd16(base + 8), tbl))
    return sorted(out)


if __name__ == "__main__":
    import sys
    from xref import Xref
    x = Xref(sys.argv[1])
    dp = decomp_profiles(sys.argv[2])
    wp = wwhd_profiles(x)
    hit = [p for p in wp if p[1] in dp]
    print("decomp profiles", len(dp), "WWHD profiles", len(wp), "same proc name", len(hit))
    for base, pn, tbl in hit[:5]:
        print(hex(base), hex(pn), dp[pn][0], ["%08X %s" % (a, n) for a, n in zip(tbl, dp[pn][1])])


def match_profiles(x, tww, named):
    """WWHD profile -> decompiled actor. `named`: address -> (name, file) from earlier stages.
    Process names shift between the versions (actors added/removed), so the shift is learned from
    profiles whose functions are already named, and applied where the nearest anchors on both sides
    agree."""
    dp = decomp_profiles(tww)
    wp = wwhd_profiles(x)
    file2id = {}
    for pid, (f, _) in sorted(dp.items()):
        file2id.setdefault(f, pid)
    anchors = []
    for base, pid, tbl in wp:
        lo, hi = min(tbl), max(tbl)
        fs = {named[a][1] for a in named if lo <= a <= hi} & set(file2id)
        if len(fs) == 1:
            anchors.append((pid, file2id[fs.pop()] - pid))
    anchors.sort()
    # drop anchors disagreeing with both neighbours (wrong earlier names)
    good = [a for i, a in enumerate(anchors)
            if (i > 0 and anchors[i - 1][1] == a[1]) or (i + 1 < len(anchors) and anchors[i + 1][1] == a[1])]
    out = {}
    ids = [a[0] for a in good]
    import bisect
    for base, pid, tbl in wp:
        i = bisect.bisect_left(ids, pid)
        before = good[i - 1][1] if i > 0 else None
        after = good[i][1] if i < len(good) else None
        if good and i < len(good) and good[i][0] == pid:
            off = good[i][1]
        elif before is not None and before == after:
            off = before
        else:
            continue
        g = dp.get(pid + off)
        if g:
            out[base] = (g[0], list(zip(tbl, g[1])))
    return out, len(good)
