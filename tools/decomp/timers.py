"""Frame-count timers: fields decremented by one (or passed to cLib_calcTimer) in code reachable from
an actor's Execute, in the GameCube build. With the WWHD counterpart when the function is matched:
the WWHD decrement sites of the matched function are paired with the GameCube ones in order.

Usage: timers.py wwhd_to_gc.tsv cking.rpx tww > timers.tsv
"""
import re
import sys
from collections import defaultdict, deque

from binmodel import GC, refine_functions
from layout import header_fields
from xref import Xref

LOAD = {32: 4, 33: 4, 34: 1, 35: 1, 40: 2, 41: 2, 42: 2, 43: 2}
STORE = {36: 4, 37: 4, 38: 1, 39: 1, 44: 2, 45: 2}


def decrements(words, calctimer=lambda i: False):
    """[(offset, width, kind)] for `this->f -= 1` (load, addi -1, store back) and
    cLib_calcTimer(&this->f) through r3 or its copies"""
    alias = {3}
    last_load = {}   # reg -> (offset, width) loaded from this
    dec = {}         # reg -> (offset, width) holding loaded-1
    addr = {}        # reg -> offset (addi rX, this, off)
    out = []
    for i, w in enumerate(words):
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        simm = (w & 0xFFFF) - (0x10000 if w & 0x8000 else 0)
        if op in LOAD and ra in alias:
            last_load[rd] = (simm, LOAD[op])
            dec.pop(rd, None)
            if rd in alias:
                alias.discard(rd)
            continue
        if op == 14 and ra in last_load and simm == -1:
            dec[rd] = last_load[ra]
            continue
        if op == 14 and ra in alias:
            addr[rd] = simm
            continue
        if op in STORE and ra in alias and rd in dec and dec[rd][0] == simm:
            out.append((simm, STORE[op], "dec"))
            dec.pop(rd)
            continue
        if op == 18 and (w & 1):
            if calctimer(i) and 3 in addr:
                out.append((addr[3], 0, "cLib_calcTimer"))
            alias &= set(range(14, 32))
            last_load.clear()
            dec.clear()
            addr.clear()
            continue
        if op == 31 and ((w >> 1) & 0x3FF) == 444 and rd == (w >> 11) & 31:
            if rd in alias:
                alias.add(ra)
            continue
        if op in (7, 12, 13, 15, 31) or (op == 14):
            last_load.pop(rd, None)
            addr.pop(rd, None)
        elif op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
            last_load.pop(ra, None)
            addr.pop(ra, None)
    return out


def main():
    tsv, rpx, tww = sys.argv[1:4]
    g = GC(tww)
    x = Xref(rpx)
    refine_functions(x)
    p = x.p
    size = {a: (x.funcs[i + 1] if i + 1 < len(x.funcs) else p.text_hi) - a for i, a in enumerate(x.funcs)}
    w_of = defaultdict(list)
    for line in list(open(tsv))[1:]:
        a, sym, *_ = line.split("\t")
        w_of[sym].append(int(a, 16))
    timer_ids = {f.id for f in g.funcs if f.name.startswith("cLib_calcTimer")}
    # roots: Execute entries of actor method tables, plus execute methods
    roots = set()
    for nm, file, sl in g.ptables:
        if "Method" in nm and len(sl) == 5:
            roots.add(sl[2][1])
    for f in g.funcs:
        if re.match(r"^_?execute__|^Execute__|^daPy_lk_c_Execute|^camera_execute", f.name):
            roots.add(f.id)
    # state/procedure tables (PTMF arrays) of the same file are reached through the Execute too
    tables = defaultdict(set)
    for nm, file, sl in g.ptables:
        if not nm.startswith("__vt__") and "Method" not in nm:
            tables[file].update(t for _, t in sl)
    seen = set(roots)
    q = deque(roots)
    files_done = set()
    while q:
        c = q.popleft()
        fl = g.funcs[c].file
        extra = []
        if fl not in files_done:
            files_done.add(fl)
            extra = [t for t in tables.get(fl, ()) if g.funcs[t].file == fl]
        for t in list(g.funcs[c].callees) + list(g.funcs[c].frefs) + extra:
            if t not in seen and g.funcs[t].file == g.funcs[c].file:
                seen.add(t)
                q.append(t)
    hdr_cache = {}
    print("file\tclass\tgc_offset\twwhd_offset\twidth\tkind\tfield\tgc_function\twwhd_function")
    for c in sorted(seen):
        f = g.funcs[c]
        # call sites to cLib_calcTimer, by word index (callee list order = relocation order)
        sites = {i for i, t in f.callsites if t in timer_ids}
        ds = decrements(f.words, lambda i: i in sites)
        if not ds:
            continue
        if c in timer_ids:
            continue
        cls = None
        if "::" in f.dem:
            cls = f.dem.split("::")[-2]
        else:
            m = re.search(r"__FP(\d+)(\w+)", f.name)
            if m:
                cls = m.group(2)[:int(m.group(1))]
        if cls not in hdr_cache:
            hdr_cache[cls] = header_fields(tww, cls)[0] if cls else {}
        fields = hdr_cache[cls]
        was = w_of.get(f.name, [])
        wd = []
        if len(was) == 1 and was[0] in size:
            ww = [p.word(k) for k in range(was[0], was[0] + size[was[0]], 4)]
            wd = [d for d in decrements(ww) if d[2] == "dec"]
        gdec = [d for d in ds if d[2] == "dec"]
        for d in ds:
            wo = "-"
            if d[2] == "dec" and len(wd) == len(gdec):
                wo = "0x%04X" % wd[gdec.index(d)][0]
            print("%s\t%s\t0x%04X\t%s\t%d\t%s\t%s\t%s\t%s" % (
                f.file, cls or "-", d[0], wo, d[1], d[2], fields.get(d[0], ""), f.dem,
                "%08X" % was[0] if len(was) == 1 else "-"))


if __name__ == "__main__":
    main()
