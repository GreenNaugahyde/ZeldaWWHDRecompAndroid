"""Reports written next to the names TSV by match.py:

  wwhd_to_gc.tsv   WWHD address -> GameCube mangled symbol, source file, module, GameCube address,
                   evidence, WWHD size, GameCube size (input for checking functions one by one)
  coverage.tsv     per translation unit: GameCube functions, how many are matched, WWHD functions
                   named after it
  regions.tsv      long stretches of WWHD code with no match (HD-only code: new UI, sead/agl,
                   nw4f libraries, GX2 renderer), used for the "no GameCube counterpart" estimate
"""
import os
import re
from collections import Counter, defaultdict

SYM_RE = re.compile(r"^(\S+) = \.text:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")
MIN_GAP = 150   # unnamed functions in a row that count as an HD-only stretch


def gc_addresses(tww):
    """(module, mangled name) -> [(address, size)] from the decompilation's symbol lists"""
    base = os.path.join(tww, "config", "GZLE01")
    out = defaultdict(list)

    def load(path, module):
        if not os.path.exists(path):
            return
        for line in open(path):
            m = SYM_RE.match(line)
            if m:
                out[(module, m.group(1))].append((int(m.group(2), 16), int(m.group(3), 16)))
    load(os.path.join(base, "symbols.txt"), "main")
    rels = os.path.join(base, "rels")
    for r in sorted(os.listdir(rels)):
        load(os.path.join(rels, r, "symbols.txt"), r)
    return out


def write_reports(x, g, m, out, tww, outdir):
    addrs = gc_addresses(tww)
    bydf = defaultdict(list)
    for f in g.funcs:
        bydf[(f.dem, f.file)].append(f.id)

    def gc_of(a):
        if a in m.M:
            return m.M[a]
        name, file = out[a][0].split(" (inlines")[0], out[a][1]
        ids = bydf.get((name, file))
        return ids[0] if ids and len(ids) == 1 else None

    def gc_addr(f):
        lst = addrs.get((f.module, f.name), [])
        hit = [ad for ad, sz in lst if sz == f.size] or [ad for ad, _ in lst]
        if not hit:
            return "-"
        return ("%08X" % hit[0]) if f.module == "main" else ("%s+%X" % (f.module, hit[0]))

    sizes = {a: (x.funcs[i + 1] if i + 1 < len(x.funcs) else x.p.text_hi) - a for i, a in enumerate(x.funcs)}
    with open(os.path.join(outdir, "wwhd_to_gc.tsv"), "w") as o:
        o.write("wwhd\tgc_symbol\tfile\tgc_module\tgc_address\tevidence\twwhd_size\tgc_size\n")
        for a in sorted(out):
            c = gc_of(a)
            if c is None:
                continue
            f = g.funcs[c]
            o.write("%08X\t%s\t%s\t%s\t%s\t%s\t%d\t%d\n" % (a, f.name, f.file, f.module, gc_addr(f), out[a][2],
                                                           sizes.get(a, 0), f.size))
    # per translation unit
    tot, hit, wn = Counter(), Counter(), Counter()
    module = {}
    for f in g.funcs:
        if m.canon[f.id] == f.id:
            tot[f.file] += 1
            module.setdefault(f.file, f.module)
    matched = set(m.Minv)
    for a in out:
        c = gc_of(a)
        if c is not None:
            matched.add(m.canon[c])
        wn[out[a][1]] += 1
    for c in matched:
        hit[g.funcs[c].file] += 1
    with open(os.path.join(outdir, "coverage.tsv"), "w") as o:
        o.write("file\tmodule\tgc_functions\tgc_matched\tpercent\twwhd_named\n")
        for f in sorted(tot, key=lambda f: (-(tot[f] - hit[f]), f)):
            o.write("%s\t%s\t%d\t%d\t%.0f\t%d\n" % (f, module[f], tot[f], hit[f], 100.0 * hit[f] / tot[f], wn[f]))
    # unnamed stretches
    F = x.funcs
    gaps, run = [], []
    for a in F:
        if a in out:
            if len(run) >= MIN_GAP:
                gaps.append(run)
            run = []
        else:
            run.append(a)
    if len(run) >= MIN_GAP:
        gaps.append(run)
    with open(os.path.join(outdir, "regions.tsv"), "w") as o:
        o.write("start\tend\tfunctions\n")
        for r in gaps:
            o.write("%08X\t%08X\t%d\n" % (r[0], r[-1], len(r)))
    in_gaps = sum(len(r) for r in gaps)
    gc_unique = sum(tot.values())
    print("GameCube functions (weak copies merged) %d, matched %d (%.1f%%)" % (gc_unique, len(matched),
                                                                             100.0 * len(matched) / gc_unique))
    print("WWHD functions %d: named %d; in unnamed stretches of >= %d functions (HD-only code) %d; "
          "other unnamed %d" % (len(F), len(out), MIN_GAP, in_gaps, len(F) - len(out) - in_gaps))
