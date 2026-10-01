"""Match WWHD (cking.rpx) functions to GameCube decompilation functions.

Evidence, strongest first:
  asserts   (file, condition) pairs shared with the decompilation's JUT_ASSERTs
  strings   string literals (direct or via file-scope constants), weighted by rarity
Every candidate must also agree with the source file of its WWHD neighbourhood: functions of one
translation unit sit together in the executable, and many reference their own file name.

Usage: match.py game/code/cking.rpx tww out.tsv
"""
import bisect
import re
import sys
from collections import Counter

from decomp_index import float_index, float_key, index, norm_cond, string_index
from profiles import match_profiles
from xref import Xref

SRC_RE = re.compile(r"^[\w.+-]+\.(cpp|h|inc)$")


def wwhd_asserts(x, f):
    out, prev = [], None
    for _, _, s in x.strings(f):
        if SRC_RE.match(s) and prev is not None and not SRC_RE.match(prev):
            out.append((s, norm_cond(prev)))
        prev = s
    return out


class TuMap:
    """source file of WWHD address ranges, from functions that name exactly one .cpp file"""

    def __init__(self, x):
        self.addr, self.tu = [], []
        for f in x.funcs:
            fs = {s for _, _, s in x.strings(f) if s.endswith(".cpp") and SRC_RE.match(s)}
            if len(fs) == 1:
                self.addr.append(f)
                self.tu.append(fs.pop())

    def add(self, labels):
        """extra (address, source file) labels, e.g. from already named functions"""
        pairs = sorted(set(zip(self.addr, self.tu)) | set(labels))
        self.addr = [a for a, _ in pairs]
        self.tu = [t for _, t in pairs]

    def around(self, f, k=3):
        """source files of the k labelled functions before and after f"""
        i = bisect.bisect_left(self.addr, f)
        return set(self.tu[max(0, i - k):i + k])


def main():
    x = Xref(sys.argv[1])
    from binmodel import refine_functions
    print("function entries: %d discovered + %d tail-call targets" % (len(x.funcs), refine_functions(x)))
    aidx, _ = index(sys.argv[2])
    sidx = string_index(sys.argv[2])
    for k, v in float_index(sys.argv[2]).items():  # float constants vote like strings
        sidx.setdefault(k, set()).update(v)
    tus = TuMap(x)

    wdf, wstr = Counter(), {}
    for f in x.funcs:
        ss = {s.encode("shift_jis", errors="replace") for _, _, s in x.strings(f) if len(s) >= 3}
        ss |= {float_key(abs(v)) for v in x.floats(f)}
        if ss:
            wstr[f] = ss
            wdf.update(ss)

    def stage(tus, keep):
        cands = {}  # f -> Counter((file, name) -> score)
        kind = {}
        for f in x.funcs:
            v = Counter()
            for k in set(wwhd_asserts(x, f)):
                for owner, _ in aidx.get(k, ()):
                    v[(k[0], owner)] += 3.0
            if v:
                kind[f] = "assert"
            for lit in wstr.get(f, ()):
                owners = sidx.get(lit)
                if not owners or len(owners) > 8 or wdf[lit] > 8:
                    continue
                w = 1.0 / (len(owners) * wdf[lit])
                for o in owners:
                    v[o] += w
            if v:
                cands[f] = v
                kind.setdefault(f, "strings")

        names, rejected_tu, ambiguous = {}, 0, 0
        for f, v in cands.items():
            near = tus.around(f)
            # keep candidates from a neighbouring source file when the neighbourhood is known
            if near:
                ok = Counter({k: s for k, s in v.items() if k[0] in near})
                if not ok:
                    rejected_tu += 1
                    continue
                v = ok
            ranked = v.most_common(2)
            (best, sc) = ranked[0]
            if sc < 0.5 or (len(ranked) > 1 and ranked[1][1] > 0.6 * sc):
                ambiguous += 1
                continue
            names[f] = (best, sc)

        # a decompiled function claimed several times: keep the best-scoring claim
        best_for = {}
        for f, (k, sc) in names.items():
            if k not in best_for or sc > names[best_for[k]][1]:
                best_for[k] = f
        dropped = len(names) - len(best_for)
        names = {f: names[f] for f in best_for.values()}

        print("asserts/strings: candidates %d -> named %d (wrong file %d, ambiguous %d, duplicate claims %d)" %
              (len(cands), len(names), rejected_tu, ambiguous, dropped))


        return names, cands, kind, rejected_tu, ambiguous, dropped

    # pass 1: asserts/strings/floats with file labels from the executable only
    names, cands, kind, *_ = stage(tus, {})
    out = {f: (name, file, kind[f], "%.2f" % sc) for f, ((file, name), sc) in names.items()}
    prof, nanch = match_profiles(x, sys.argv[2], {f: (n, fl) for f, (n, fl, _, _) in out.items()})
    # pass 2: profiles place ~300 actors' code; with those file labels the neighbourhood check
    # accepts candidates it could not judge before
    tus.add([(a, file) for _b, (file, lst) in prof.items() for a, _n in lst])
    names, cands, kind, rejected_tu, ambiguous, dropped = stage(tus, {})
    # stage 3: actor profiles (Create/Delete/Execute/IsDelete/Draw of every matched actor)
    out = {f: (name, file, kind[f], "%.2f" % sc) for f, ((file, name), sc) in names.items()}
    prof, nanch = match_profiles(x, sys.argv[2], {f: (n, fl) for f, (n, fl, _, _) in out.items()})
    added = 0
    for _base, (file, lst) in prof.items():
        for a, name in lst:
            if a in out and out[a][0] != name:
                # wrapper with the named function inlined into it
                out[a] = ("%s (inlines %s)" % (name, out[a][0]), file, "profile", "-")
            elif a not in out:
                out[a] = (name, file, "profile", "-")
                added += 1
    print("profiles: %d matched (%d anchors) -> %d more functions" % (len(prof), nanch, added))
    # stage 4: whole-program matching against the GameCube binary (match2: function-pointer tables,
    # call graph + content, translation-unit gaps, inline copies). Needs the decompilation built from
    # your own disc image: tww/build/GZLE01.
    import os
    pairs = None
    if os.path.isdir(os.path.join(sys.argv[2], "build", "GZLE01")):
        from binmodel import GC, WW
        from heldout import resolve_seeds
        from match2 import Matcher
        g = GC(sys.argv[2])
        m = Matcher(g, WW(x), log=print)
        manual = m.load_manual(os.path.join(os.path.dirname(os.path.abspath(__file__)), "manual_names.tsv"))
        for a, c in m.M.items():
            f = g.funcs[c]
            out[a] = (f.dem, f.file, "manual", "-")
        print("manual names: %d" % manual)
        rows = [(a, n, fl, k) for a, (n, fl, k, _s) in out.items() if k != "manual"]
        seeds = resolve_seeds(m, rows, {"assert", "strings", "profile"})
        for a, c in seeds.items():
            m.add(a, c, out[a][2])
        m.run()
        grown = 0
        for a, c in m.M.items():
            if a not in out:
                f = g.funcs[c]
                out[a] = (f.dem, f.file, m.ev[a], "%.2f" % m.prob[a] if a in m.prob else "-")
                grown += 1
        print("match2: +%d functions from %d seeds" % (grown, len(seeds)))
        pairs = (g, m)
        # stage 5: the first call-graph matcher (callmatch.py), kept as a lower-confidence fallback
        # (~85-90% held-out precision) for functions match2 leaves open
        legacy = legacy_callgraph(x, sys.argv[2], out)
        claimed = {(g.funcs[c].dem, g.funcs[c].file) for c in m.Minv}
        added = 0
        for a, (name, file) in legacy.items():
            if a not in out and (name, file) not in claimed:
                out[a] = (name, file, "callgraph-legacy", "-")
                added += 1
        print("legacy call graph: +%d functions (lower confidence)" % added)
    print("total named %d of %d WWHD functions" % (len(out), len(x.funcs)))
    with open(sys.argv[3], "w") as o:
        for f in sorted(out):
            o.write("%08X\t%s\t%s\t%s\t%s\n" % ((f,) + out[f]))
    if pairs:
        from report import write_reports
        write_reports(x, pairs[0], pairs[1], out, sys.argv[2], os.path.dirname(os.path.abspath(sys.argv[3])))
    return x, names


def legacy_callgraph(x, tww, out):
    import math
    from callgraph import wwhd_calls
    from callmatch import propagate
    from features import gc_functions, wwhd_functions
    from gc_layout import demangle
    x._pos = {f: i for i, f in enumerate(x.funcs)}
    W = wwhd_functions(x)
    G = gc_functions(tww)
    wc = {a: wwhd_calls(x, a) for a in x.funcs}
    df = Counter()
    for g in G:
        df.update(set(g.tokens))
    for w in W.values():
        df.update(set(w.tokens))
    n = len(G) + len(W)
    idf = {k: math.log(n / (1 + c)) for k, c in df.items()}
    gm, gfile = {}, {}
    for g in G:
        gm.setdefault((demangle(g.name), g.file), g.name)
        gfile.setdefault(g.name, g.file)
    seeds = {}
    for a, (name, file, _k, _s) in out.items():
        key = (name.split(" (inlines")[0], file)
        if key in gm:
            seeds[a] = gm[key]
    m = propagate(G, W, wc, seeds, idf, log=lambda s: None)
    return {a: (demangle(gn), gfile[gn]) for a, gn in m.items() if a not in seeds}


if __name__ == "__main__":
    import os
    if os.environ.get("PYTHONHASHSEED") != "0":
        # set iteration order decides ties in the matcher; fix it so runs are reproducible
        os.environ["PYTHONHASHSEED"] = "0"
        os.execv(sys.executable, [sys.executable] + sys.argv)
    main()
