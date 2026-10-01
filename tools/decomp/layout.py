"""Infer WWHD structure layouts from matched function pairs.

For every matched pair whose GameCube function takes a pointer to class C as `this` (methods of C)
or as first argument (e.g. fopAcM_* on fopAc_ac_c*), the loads/stores through that pointer are
listed in order on both sides and aligned (same access width/kind scores, gaps allowed). Every
aligned pair votes GC offset -> WWHD offset; the majority wins. Field names come from the
decompilation headers' /* 0x... */ offset comments.

Usage: layout.py wwhd_to_gc.tsv cking.rpx tww Class [Class...]
"""
import os
import re
import sys
from collections import Counter, defaultdict

from binmodel import GC, refine_functions
from xref import Xref

# opcode -> access kind
KIND = {32: "w", 36: "w", 33: "w", 37: "w", 34: "b", 38: "b", 35: "b", 39: "b", 40: "h", 42: "h", 44: "h",
        41: "h", 43: "h", 45: "h", 48: "f", 52: "f", 49: "f", 53: "f", 50: "d", 54: "d", 51: "d", 55: "d"}


MINVOTES = int(os.environ.get("LAYOUT_MINVOTES", "3"))
BASE_SIZE = {"fopAc_ac_c": 0x290}


def accesses(words):
    """[(kind, displacement)] of loads/stores through the first argument (r3 and its copies)"""
    alias = {3}
    out = []
    for w in words:
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        simm = (w & 0xFFFF) - (0x10000 if w & 0x8000 else 0)
        if op in KIND and ra in alias:
            out.append((KIND[op], simm))
            if op < 48 and op not in (36, 37, 38, 39, 44, 45) and rd in alias:
                alias.discard(rd)   # loaded over the pointer
            continue
        if op == 14 and ra in alias and rd not in alias:
            out.append(("a", simm))   # address of a member (sub-object)
        if op == 31 and ((w >> 1) & 0x3FF) == 444 and rd == (w >> 11) & 31:   # mr rA, rS
            if rd in alias:
                alias.add(ra)
            elif ra in alias:
                alias.discard(ra)
            continue
        if op in (14, 15, 32, 33, 34, 35, 40, 41, 42, 43, 7, 12, 13):
            alias.discard(rd)
        elif op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
            alias.discard(ra)
        elif op == 31:
            alias.discard(rd)
            alias.discard(ra)
        elif op == 18 and (w & 1):
            alias &= set(range(14, 32))
    return out


def align(A, B):
    """global alignment of access lists; returns paired (offset_a, offset_b)"""
    n, m = len(A), len(B)
    if n * m > 250000:
        return []
    S = [[0.0] * (m + 1) for _ in range(n + 1)]
    for i in range(1, n + 1):
        S[i][0] = -0.5 * i
    for j in range(1, m + 1):
        S[0][j] = -0.5 * j
    for i in range(1, n + 1):
        for j in range(1, m + 1):
            s = 2.0 if A[i - 1][0] == B[j - 1][0] else -2.0
            if A[i - 1][1] == B[j - 1][1] and A[i - 1][0] == B[j - 1][0]:
                s = 2.5
            S[i][j] = max(S[i - 1][j - 1] + s, S[i - 1][j] - 0.5, S[i][j - 1] - 0.5)
    out = []
    i, j = n, m
    while i > 0 and j > 0:
        s = 2.0 if A[i - 1][0] == B[j - 1][0] else -2.0
        if A[i - 1][1] == B[j - 1][1] and A[i - 1][0] == B[j - 1][0]:
            s = 2.5
        if S[i][j] == S[i - 1][j - 1] + s:
            if A[i - 1][0] == B[j - 1][0]:
                out.append((A[i - 1][1], B[j - 1][1], A[i - 1][0]))
            i, j = i - 1, j - 1
        elif S[i][j] == S[i - 1][j] - 0.5:
            i -= 1
        else:
            j -= 1
    return out


def mangled_class(c):
    return "%d%s" % (len(c), c)


def header_fields(tww, cls):
    """GC offset -> field name, from `class cls ... { /* 0x... */ type name; ... }`"""
    out = {}
    pat = re.compile(r"\b(class|struct)\s+%s\b[^;{]*\{" % re.escape(cls))
    for d, _, files in os.walk(os.path.join(tww, "include")):
        for f in files:
            if not f.endswith(".h"):
                continue
            src = open(os.path.join(d, f), encoding="utf-8", errors="replace").read()
            m = pat.search(src)
            if not m:
                continue
            depth, i = 1, m.end()
            while i < len(src) and depth:
                depth += {"{": 1, "}": -1}.get(src[i], 0)
                i += 1
            for fm in re.finditer(r"/\*\s*0x([0-9A-Fa-f]+)\s*\*/\s*([^;]+);", src[m.end():i]):
                decl = " ".join(fm.group(2).split())
                out.setdefault(int(fm.group(1), 16), decl)
            return out, f
    return out, None


def main():
    tsv, rpx, tww = sys.argv[1:4]
    classes = sys.argv[4:]
    x = Xref(rpx)
    refine_functions(x)
    g = GC(tww)
    p = x.p
    size = {a: (x.funcs[i + 1] if i + 1 < len(x.funcs) else p.text_hi) - a for i, a in enumerate(x.funcs)}
    pairs = []
    for line in list(open(tsv))[1:]:
        a, sym, file, *_ = line.split("\t")
        pairs.append((int(a, 16), sym, file))
    for cls in classes:
        mc = mangled_class(cls)
        votes = defaultdict(Counter)
        npairs = 0
        for a, sym, file in pairs:
            meth = re.search(r"__%sF" % re.escape(mc), sym) and not sym.startswith(("__ct", "__dt"))
            first = re.search(r"__F(P|R)%s" % re.escape(mc), sym)
            # actor base class: every actor method / function on an actor pointer
            # (only offsets inside fopAc_ac_c are kept below)
            derived = cls == "fopAc_ac_c" and file.startswith("d_a_") and (
                re.search(r"__\d+\w+F", sym) or re.search(r"__FP\d+\w+", sym))
            if not (meth or first or derived):
                continue
            ids = g.byname.get(sym)
            if not ids or a not in size:
                continue
            gw = g.funcs[ids[0]].words
            ww = [p.word(k) for k in range(a, a + size[a], 4)]
            A, B = accesses(gw), accesses(ww)
            if not A or not B:
                continue
            npairs += 1
            for go, wo, k in align(A, B):
                if derived and not (meth or first) and go >= BASE_SIZE.get(cls, 1 << 30):
                    continue
                votes[(go, k)][wo] += 1
        fields, hdr = header_fields(tww, cls)
        print("== %s (%s): %d function pairs" % (cls, hdr, npairs))
        rows = []
        for (go, k), v in votes.items():
            wo, n = v.most_common(1)[0]
            tot = sum(v.values())
            if n >= MINVOTES and n >= 0.6 * tot and go >= 0:
                rows.append((go, k, wo, n, tot))
        rows.sort()
        seen = set()
        for go, k, wo, n, tot in rows:
            if (go, wo) in seen:
                continue
            seen.add((go, wo))
            name = fields.get(go, "")
            print("  gc 0x%04X -> wwhd 0x%04X  (%+d)  %s  votes %d/%d  %s" % (go, wo, wo - go, k, n, tot, name))


if __name__ == "__main__":
    main()
