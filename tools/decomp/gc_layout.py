"""GameCube layout from the decompilation's config (tww/config/GZLE01): for every source file
(translation unit), its functions in link order with sizes. Covers main.dol and all RELs.
"""
import os
import re

SYM_RE = re.compile(r"^(\S+) = \.text:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)")
SPLIT_FILE_RE = re.compile(r"^(\S.*):$")
SPLIT_TEXT_RE = re.compile(r"^\s+\.text\s+start:0x([0-9A-Fa-f]+) end:0x([0-9A-Fa-f]+)")


def demangle(m):
    """CodeWarrior mangled name -> 'Class::method' (best effort; plain names unchanged)"""
    i = m.find("__", 1)
    while i > 0 and not (i + 2 < len(m) and (m[i + 2].isdigit() or m[i + 2] in "QF")):
        i = m.find("__", i + 1)
    if i <= 0:
        return m
    name, rest = m[:i], m[i + 2:]
    parts = []
    if rest.startswith("Q") and len(rest) > 1 and rest[1].isdigit():
        n, rest = int(rest[1]), rest[2:]
        if rest.startswith("_"):
            rest = rest[1:]
        for _ in range(n):
            k = re.match(r"(\d+)", rest)
            if not k:
                break
            ln = int(k.group(1))
            parts.append(rest[len(k.group(1)):len(k.group(1)) + ln])
            rest = rest[len(k.group(1)) + ln:]
    elif rest[:1].isdigit():
        k = re.match(r"(\d+)", rest)
        ln = int(k.group(1))
        parts.append(rest[len(k.group(1)):len(k.group(1)) + ln])
    if name == "__ct" and parts:
        name = parts[-1].split("<")[0]
    elif name == "__dt" and parts:
        name = "~" + parts[-1].split("<")[0]
    return "::".join(parts + [name])


def _module(cfg_dir):
    splits = os.path.join(cfg_dir, "splits.txt")
    syms = os.path.join(cfg_dir, "symbols.txt")
    if not (os.path.exists(splits) and os.path.exists(syms)):
        return {}
    ranges = []  # (start, end, file)
    cur = None
    for line in open(splits):
        m = SPLIT_FILE_RE.match(line)
        if m and not line.startswith(("Sections", "\t")):
            cur = m.group(1)
            continue
        m = SPLIT_TEXT_RE.match(line)
        if m and cur:
            ranges.append((int(m.group(1), 16), int(m.group(2), 16), cur))
    funcs = []
    for line in open(syms):
        m = SYM_RE.match(line)
        if m:
            funcs.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    funcs.sort()
    out = {}
    for a, sz, name in funcs:
        for s, e, f in ranges:
            if s <= a < e:
                out.setdefault(os.path.basename(f), []).append((a, sz, name))
                break
    return out


def layout(tww):
    base = os.path.join(tww, "config", "GZLE01")
    tus = _module(base)
    rels = os.path.join(base, "rels")
    for r in sorted(os.listdir(rels)):
        for f, lst in _module(os.path.join(rels, r)).items():
            if f == "executor.c":
                continue
            tus.setdefault(f, []).extend(lst)
    return tus


if __name__ == "__main__":
    import sys
    t = layout(sys.argv[1])
    print("translation units", len(t), "functions", sum(len(v) for v in t.values()))
    for f in ("c_bg_s.cpp", "d_a_bflower.cpp"):
        print(f, [(hex(a), sz, demangle(n)) for a, sz, n in t.get(f, [])][:6])
