"""Spread names through the call graph.

For a WWHD function already named after decompiled function F, compare F's calls (from the
source) with the WWHD function's calls (bl targets). Callees that are resolved on both sides are
paired off; when exactly one unresolved callee remains on each side, they are the same function.
Repeated until nothing changes. Conservative on purpose: inlining differs between the compilers,
so anything less certain is left unnamed.
"""
import os
import re

from decomp_index import mask, scan

KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "do", "else", "case", "new", "delete",
            "static_cast", "reinterpret_cast", "const_cast", "dynamic_cast", "defined", "offsetof"}
CALL_RE = re.compile(r"(?:(->|\.)\s*)?([A-Za-z_]\w*(?:::[A-Za-z_~]\w*)*)\s*\(")


def decomp_calls(tww):
    """(file, function) -> ordered list of callee names as written (last component for members)"""
    out = {}
    for d, _, files in os.walk(os.path.join(tww, "src")):
        for f in files:
            if not f.endswith(".cpp"):
                continue
            path = os.path.join(d, f)
            funcs, _ = scan(path)
            text = mask(open(path, encoding="utf-8", errors="replace").read())
            for name, a, b in funcs:
                calls = []
                for m in CALL_RE.finditer(text, a + 1, b):
                    callee = m.group(2)
                    if callee in KEYWORDS or callee.isupper():
                        continue
                    calls.append(callee.split("::")[-1] if m.group(1) else callee)
                out[(f, name)] = calls
    return out


def wwhd_calls(x, f):
    """bl targets of WWHD function f in order (internal functions only)"""
    p = x.p
    i = x.funcs.index(f) if not hasattr(x, "_pos") else x._pos[f]
    end = x.funcs[i + 1] if i + 1 < len(x.funcs) else p.text_hi
    out = []
    for a in range(f, end, 4):
        w = p.word(a)
        if (w >> 26) == 18 and (w & 1) and a not in p.import_calls and a not in p.undef_calls:
            t = ((w & 0x03FFFFFC) - (0x04000000 if w & 0x02000000 else 0) + (0 if w & 2 else a)) & 0xFFFFFFFF
            if p.in_text(t):
                out.append(t)
    return out


def spread(x, tww, named):
    """named: WWHD address -> (decomp name, file). Returns new names {address: (name, file)}."""
    x._pos = {f: i for i, f in enumerate(x.funcs)}
    calls = decomp_calls(tww)
    by_file = {}  # file -> {short name -> [full names]}
    for (f, name) in calls:
        by_file.setdefault(f, {}).setdefault(name.split("::")[-1], []).append(name)
        by_file[f].setdefault(name, []).append(name)
    taken = {(fl, n) for n, fl in named.values()}
    new = {}
    changed = True
    while changed:
        changed = False
        cur = dict(named)
        cur.update(new)
        for f, (name, file) in list(cur.items()):
            dcalls = calls.get((file, name.split(" (inlines")[0]))
            if not dcalls:
                continue
            local = by_file.get(file, {})
            # decompiled callees defined in the same file, resolved uniquely
            dres = []
            for c in dcalls:
                full = local.get(c)
                if full and len(set(full)) == 1:
                    dres.append(full[0])
            dun = [c for c in dict.fromkeys(dres) if (file, c) not in taken]
            wun = [t for t in dict.fromkeys(wwhd_calls(x, f)) if t not in cur]
            if len(dun) == 1 and len(wun) == 1:
                new[wun[0]] = (dun[0], file)
                taken.add((file, dun[0]))
                changed = True
    return new
