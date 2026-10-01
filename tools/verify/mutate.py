#!/usr/bin/env python3
"""Mutation test of the harness: how many deliberately wrong candidates does it reject?

usage: mutate.py UNIT [--n 300] [--max 400] [--seed 1]

For every function body in the unit's candidate sources (from WWHD_FUNC to the next VERIFY),
make single-point mutants: change a literal, flip a comparison or logical operator, drop a
statement, swap two adjacent statements, change a + to -. Each mutant is compiled into the
unit and run with -n N generated inputs on its own function. A mutant that still passes is a
survivor: either equivalent to the original (e.g. swapping two independent stores) or a gap
in the inputs. Survivors are listed for inspection.
"""
import argparse
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import verify  # noqa: E402


def bodies(text):
    """(addr, start, end): the code of each verified function, including the helpers defined
    between the previous VERIFY line and its own (inlined GameCube functions)"""
    out = []
    prev = 0
    for m in re.finditer(r"WWHD_FUNC\(\s*0x([0-9A-Fa-f]{8})[^;]*;", text):
        end = text.find("\nVERIFY(", m.end())
        end = end if end > 0 else len(text)
        # helpers: from the previous VERIFY line (or the first function definition) on
        start = prev if prev else text.rfind("\n", 0, text.rfind("\n", 0, m.start()))
        out.append((int(m.group(1), 16), max(start, 0), end))
        prev = text.find("\n", end + 1)
    return out


def mutants(text, start, end):
    # blank out comments (keeping offsets) so mutations only hit code
    # (and the WWHD_FUNC/VERIFY bookkeeping, which only names the function)
    body = re.sub(r"/\*.*?\*/|//[^\n]*|WWHD_FUNC\([^;]*;|VERIFY\([^;]*;", lambda m: re.sub(r"[^\n]", " ", m.group(0)),
                  text[start:end], flags=re.S)
    res = []
    for m in re.finditer(r"(?<![\w.])(\d+\.\d+f|\d+)(?![\w.x])", body):
        lit = m.group(1)
        if lit.endswith("f"):
            new = repr(float(lit[:-1]) + 1.0) + "f"
        else:
            new = str(int(lit) + 1)
        res.append(("literal %s->%s" % (lit, new), start + m.start(1), start + m.end(1), new))
    for m in re.finditer(r"0x([0-9A-Fa-f]+)", body):
        v = int(m.group(1), 16)
        res.append(("literal 0x%X->0x%X" % (v, v ^ 1), start + m.start(), start + m.end(), "0x%X" % (v ^ 1)))
    flips = {"==": "!=", "!=": "==", "<=": "<", ">=": ">", "<": "<=", ">": ">=", "&&": "||", "||": "&&"}
    for m in re.finditer(r"==|!=|<=|>=|&&|\|\||(?<![<>-])[<>](?![<>=])", body):
        op = m.group(0)
        res.append(("operator %s->%s" % (op, flips[op]), start + m.start(), start + m.end(), flips[op]))
    for m in re.finditer(r"(?<=[\w)\]]) \+ (?=[\w(])", body):
        res.append(("operator + -> -", start + m.start(), start + m.end(), " - "))
    for m in re.finditer(r"!(?=[\w(])", body):
        res.append(("drop !", start + m.start(), start + m.end(), ""))
    lines = []
    pos = start
    for ln in body.split("\n"):
        s = ln.strip()
        if not s or s.startswith("#") or "/* 0x" in s or s.startswith(("VERIFY(", "WWHD_")):
            pos += len(ln) + 1
            continue
        if s.endswith(";") and not s.startswith(("return", "WWHD_FUNC", "gabi::Local", "//", "/*")) and "=" in s or \
                (s.endswith(";") and re.match(r"^[\w:>.\-]+\(.*\);$", s) and not s.startswith("return")):
            lines.append((pos, pos + len(ln)))
        pos += len(ln) + 1
    for a, b in lines:
        res.append(("drop statement `%s`" % text[a:b].strip()[:50], a, b, ""))
    for (a1, b1), (a2, b2) in zip(lines, lines[1:]):
        between = text[b1:a2]
        if between.strip() == "":
            res.append(("swap `%s` / `%s`" % (text[a1:b1].strip()[:30], text[a2:b2].strip()[:30]), a1, b2,
                        text[a2:b2] + between + text[a1:b1]))
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("unit")
    ap.add_argument("--n", type=int, default=300)
    ap.add_argument("--max", type=int, default=400)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--rec")
    a = ap.parse_args()
    exe = verify.build(a.unit)
    srcs = []
    for line in open(os.path.join(HERE, "units", a.unit + ".txt")):
        f = line.split("#")[0].split()
        if f and f[0] == "src":
            srcs += f[1:]
    out = os.path.join(ROOT, "build", "verify", a.unit)
    spec = os.path.join(HERE, "units", a.unit + ".txt")
    objs = [os.path.join(out, os.path.basename(s) + ".o") for s in [os.path.join(out, "unit.c"), "runtime/src/espresso_fp.c",
                                                                      "tools/verify/src/harness.cpp"] + srcs]
    inc = ["-Iruntime/include", "-Itools/verify/include", "-Itools/verify/src", "-Iwwhd_src/include"]
    allm = []
    for src in srcs:
        text = open(os.path.join(ROOT, src)).read()
        for addr, s, e in bodies(text):
            for m in mutants(text, s, e):
                allm.append((src, addr, m))
    random.Random(a.seed).shuffle(allm)
    allm = allm[:a.max]
    killed = survived = broken = 0
    surv = []
    for i, (src, addr, (desc, s, e, new)) in enumerate(allm):
        text = open(os.path.join(ROOT, src)).read()
        mt = text[:s] + new + text[e:]
        mpath = os.path.join(out, "mutant.cpp")
        open(mpath, "w").write("#line 1 \"%s\"\n" % src + mt)
        mobj = os.path.join(out, "mutant.o")
        r = subprocess.run(["clang++", "-std=c++20", "-O1", "-w"] + verify.FP + inc + ["-I" + os.path.dirname(os.path.join(ROOT, src)),
                           "-c", mpath, "-o", mobj], cwd=ROOT, capture_output=True)
        if r.returncode:
            broken += 1
            continue
        mexe = os.path.join(out, "mutant")
        lo = [o if not o.endswith(os.path.basename(src) + ".o") else mobj for o in objs]
        subprocess.run(["clang++", "-o", mexe] + lo, cwd=ROOT, check=True)
        cmd = [mexe, "-spec", spec, "-n", str(a.n)]  # whole unit: helpers can be shared
        if a.rec:
            cmd += ["-rec", a.rec]
        try:
            r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=30)
            ok = r.returncode == 0
        except subprocess.TimeoutExpired:
            ok = False
        if ok:
            survived += 1
            surv.append("%08X %s" % (addr, desc))
        else:
            killed += 1
    total = killed + survived
    print("unit %s: %d mutants compiled (%d did not compile), killed %d, survived %d (kill rate %.1f%%)" % (
        a.unit, total, broken, killed, survived, 100.0 * killed / max(1, total)))
    for s in surv:
        print("  survivor: " + s)


if __name__ == "__main__":
    main()
