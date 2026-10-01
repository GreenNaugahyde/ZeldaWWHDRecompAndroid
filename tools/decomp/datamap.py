"""WWHD data addresses of GameCube globals, learned from matched function pairs (match2.learn_data).

Usage: datamap.py wwhd_to_gc.tsv cking.rpx tww [regex] > data_map.tsv
"""
import re
import sys
from collections import defaultdict

from binmodel import GC, WW, refine_functions
from match2 import Matcher
from xref import Xref


def main():
    tsv, rpx, tww = sys.argv[1:4]
    pat = re.compile(sys.argv[4]) if len(sys.argv) > 4 else None
    g = GC(tww)
    x = Xref(rpx)
    refine_functions(x)
    m = Matcher(g, WW(x), log=lambda s: None)
    for line in list(open(tsv))[1:]:
        a, sym, *_ = line.split("\t")
        ids = g.byname.get(sym)
        if ids:
            m.M[int(a, 16)] = m.canon[ids[0]]
            m.ev[int(a, 16)] = "x"
    m.learn_data()
    by = defaultdict(list)
    for addr, sym in m.amap.items():
        by[sym].append(addr)
    print("gc_symbol\twwhd_addresses (lowest = likely base)")
    for sym in sorted(by):
        if pat and not pat.search(sym):
            continue
        print("%s\t%s" % (sym, " ".join("%08X" % a for a in sorted(by[sym])[:8])))


if __name__ == "__main__":
    main()
