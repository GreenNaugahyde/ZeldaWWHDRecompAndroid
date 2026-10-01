"""Held-out precision test for match2 stages.

Trusted pairs (assert, profile and strings evidence from match.py) are split at random: a fraction
is hidden, the matcher runs from the rest, and every hidden function it names is checked against
the hidden name. Reports precision/recall per evidence tag.

Usage: heldout.py names.tsv cking.rpx tww [fraction] [seed]
"""
import random
import sys
from collections import Counter, defaultdict

from binmodel import GC, WW, refine_functions
from features import similarity
from match2 import Matcher
from xref import Xref


def resolve_seeds(m, rows, kinds):
    """(address, demangled name, file) rows -> {address: canonical GC id} (overloads by content)"""
    g = m.g
    bydf = defaultdict(list)
    for f in g.funcs:
        bydf[(f.dem, f.file)].append(f.id)
    out = {}
    for a, name, file, ev in rows:
        if ev not in kinds:
            continue
        ids = bydf.get((name.split(" (inlines")[0], file))
        if not ids:
            continue
        if len(ids) > 1:
            ids = sorted(ids, key=lambda c: -similarity(g.funcs[c], m.W[a], m.idf))
        out[a] = m.canon[ids[0]]
    return out


def load_rows(path):
    rows = []
    for line in open(path):
        a, n, f, e, _s = line.rstrip("\n").split("\t")
        rows.append((int(a, 16), n, f, e))
    return rows


def main():
    tsv, rpx, tww = sys.argv[1:4]
    frac = float(sys.argv[4]) if len(sys.argv) > 4 else 0.2
    rnd = random.Random(int(sys.argv[5]) if len(sys.argv) > 5 else 1)
    g = GC(tww)
    x = Xref(rpx)
    refine_functions(x)
    w = WW(x)
    m = Matcher(g, w, log=lambda s: None)
    seeds = resolve_seeds(m, load_rows(tsv), {"assert", "strings", "profile"})
    # truth = seeds plus names from function-pointer tables (100% on the first held-out runs)
    for a, c in seeds.items():
        m.add(a, c, "seed")
    m.run()
    truth = {a: c for a, c in m.M.items() if m.ev[a] in ("seed", "vtable")}
    print("truth: %d seeds + %d table names" % (len(seeds), len(truth) - len(seeds)))
    seeds = truth
    m = Matcher(g, w, log=lambda s: None)
    import os
    rows = load_rows(tsv)
    kind = {a: k for a, _n, _f, k in rows}
    keys = sorted(seeds)
    if os.environ.get("HELDOUT_ONLY"):
        # hide only names of these kinds (e.g. "assert,strings": functions without table evidence)
        only = set(os.environ["HELDOUT_ONLY"].split(","))
        pool = [a for a in keys if kind.get(a) in only]
        rnd.shuffle(pool)
        hidden = set(pool[:int(len(pool) * frac)])
        keys = pool + [a for a in keys if a not in hidden]
    else:
        rnd.shuffle(keys)
    if not os.environ.get("HELDOUT_ONLY"):
        hidden = set(keys[:int(len(keys) * frac)])
    for a in keys:
        if a not in hidden:
            m.add(a, seeds[a], "seed")
    m.log = print
    m.run()
    if os.environ.get("HELDOUT_LEGACY"):
        # the fallback stage of match.py, seeded with everything match2 named
        from match import legacy_callgraph
        out = {a: (g.funcs[c].dem, g.funcs[c].file, m.ev[a], "-") for a, c in m.M.items()}
        claimed = {(g.funcs[c].dem, g.funcs[c].file) for c in m.Minv}
        bydf = defaultdict(list)
        for f in g.funcs:
            bydf[(f.dem, f.file)].append(f.id)
        for a, (name, file) in legacy_callgraph(x, tww, out).items():
            if a not in m.M and (name, file) not in claimed and bydf.get((name, file)):
                m.M[a] = m.canon[bydf[(name, file)][0]]
                m.ev[a] = "legacy"
    ok, bad, inl = Counter(), Counter(), Counter()
    for a in hidden:
        if a in m.M:
            e = m.ev[a]
            if m.M[a] == seeds[a] or g.funcs[m.M[a]].dem == g.funcs[seeds[a]].dem and g.funcs[m.M[a]].file == g.funcs[seeds[a]].file:
                ok[e] += 1
            elif m.M[a] in m.gcallees.get(seeds[a], ()) or seeds[a] in m.gcallees.get(m.M[a], ()):
                inl[e] += 1   # wrapper vs. its callee: one of them is inlined into the other in WWHD
            else:
                bad[e] += 1
                if bad[e] <= 5:
                    print("  wrong %s %08X: %s (%s) should be %s (%s)" % (e, a, g.funcs[m.M[a]].name, g.funcs[m.M[a]].file,
                                                                       g.funcs[seeds[a]].name, g.funcs[seeds[a]].file))
    print("hidden %d" % len(hidden))
    for e in sorted(set(ok) | set(bad) | set(inl)):
        n = ok[e] + bad[e] + inl[e]
        print("  %-8s named %4d  correct %4d  wrapper/inlined callee %3d  wrong %3d  precision %.1f%% (%.1f%% counting"
              " wrapper/callee as wrong)" % (e, n, ok[e], inl[e], bad[e], 100.0 * (ok[e] + inl[e]) / n, 100.0 * ok[e] / n))
    tot = sum(ok.values()) + sum(bad.values()) + sum(inl.values())
    print("  all      named %4d of %d hidden (recall %.1f%%), precision %.1f%%" % (
        tot, len(hidden), 100.0 * tot / len(hidden), 100.0 * (sum(ok.values()) + sum(inl.values())) / max(1, tot)))
    print("total named", len(m.M), Counter(m.ev.values()))


if __name__ == "__main__":
    main()
