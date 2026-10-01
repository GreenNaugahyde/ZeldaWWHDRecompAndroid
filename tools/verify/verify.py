#!/usr/bin/env python3
"""Build and run a verification unit.

usage: verify.py UNIT [harness options...]      e.g. verify.py d_a_mtoge -n 2000 -rec build/verify/rec

Steps: mkimage (once), mkunit (unit.c from build/gen), compile unit.c + the candidate sources +
the harness with exact FP flags, run. Harness options: -n N generated inputs per function,
-seed S, -rec DIR recorded inputs (DIR/ADDR/*.tap), -only ADDR, -v / -v -v.
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
FP = ["-ffp-contract=off", "-fno-fast-math", "-fno-strict-aliasing"]


def sh(cmd):
    r = subprocess.run(cmd, cwd=ROOT)
    if r.returncode:
        sys.exit(r.returncode)


def build(unit):
    out = os.path.join(ROOT, "build", "verify", unit)
    image = os.path.join(ROOT, "build", "verify", "image.bin")
    if not os.path.exists(image):
        sh([sys.executable, os.path.join(HERE, "mkimage.py"), "game/code/cking.rpx", image])
    sh([sys.executable, os.path.join(HERE, "mkunit.py"), unit, "--root", ROOT])
    srcs = []
    for line in open(os.path.join(HERE, "units", unit + ".txt")):
        f = line.split("#")[0].split()
        if f and f[0] == "src":
            srcs += f[1:]
    inc = ["-Iruntime/include", "-Itools/verify/include", "-Itools/verify/src", "-Iwwhd_src/include"]
    objs = []
    for src, lang in [(os.path.join(out, "unit.c"), "c"), ("runtime/src/espresso_fp.c", "c"), ("tools/verify/src/harness.cpp", "c++")] + \
            [(s, "c++") for s in srcs]:
        obj = os.path.join(out, os.path.basename(src) + ".o")
        objs.append(obj)
        if os.path.exists(obj) and os.path.getmtime(obj) > max(os.path.getmtime(os.path.join(ROOT, src)), newest_header()):
            continue
        if lang == "c":
            cmd = ["clang", "-std=c11", "-O2", "-w"] + FP + inc + ["-c", src, "-o", obj]
        else:
            cmd = ["clang++", "-std=c++20", "-O2", "-Wall", "-Wno-invalid-offsetof", "-Wno-unused-function"] + FP + inc + ["-c", src, "-o", obj]
        sh(cmd)
    exe = os.path.join(out, "verify")
    sh(["clang++", "-o", exe] + objs)
    return exe


def newest_header():
    t = 0
    for d in ("tools/verify/include", "tools/verify/src", "wwhd_src/include", "runtime/include"):
        for dp, _, fs in os.walk(os.path.join(ROOT, d)):
            for f in fs:
                t = max(t, os.path.getmtime(os.path.join(dp, f)))
    return t


def main():
    unit = sys.argv[1]
    exe = build(unit)
    spec = os.path.join(HERE, "units", unit + ".txt")
    r = subprocess.run([exe, "-spec", spec] + sys.argv[2:], cwd=ROOT)
    sys.exit(r.returncode)


if __name__ == "__main__":
    main()
