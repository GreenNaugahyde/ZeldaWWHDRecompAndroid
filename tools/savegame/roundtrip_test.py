#!/usr/bin/env python3
"""Round trip check: HD cking.sav -> GameCube-layout save data -> HD, field by field.

usage: roundtrip_test.py cking.sav [...]

Every shared field (all fields of the packed save data, see wwsave.FIELD_NAMES) must come back
bit-identical; the .gci produced in between must pass the GameCube checksums. HD-only data (the
per-file UTF-16 name and HD area) is not part of the GameCube layout and is reported, not compared.
Works on copies in memory only; the input files are opened read-only.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wwsave as W  # noqa: E402
import gc2hd  # noqa: E402
import hd2gc  # noqa: E402


def check(path):
    raw = open(path, "rb").read()
    hd_slots, hd_extras, _ = W.read_hd(raw)
    gci = W.write_gci(b"GZLE01", hd2gc.hd_to_gc_slots(raw))
    code, gc_slots = W.read_gci(gci)                     # GameCube checksums verified here
    assert all(g is not None for g in gc_slots)
    _, back, _ = gc2hd.convert_gci(gci)
    back_slots, back_extras, _ = W.read_hd(back)        # HD checksums + CRC verified here
    bad = 0
    for s in range(3):
        diffs = [f for f in W.FIELD_NAMES
                 if W.get(hd_slots[s], W.HD_FIELDS, f) != W.get(back_slots[s], W.HD_FIELDS, f)]
        bad += len(diffs)
        hd_only = [k for k in hd_extras[s] if (W.hd_name(hd_extras[s]) if k == "name" else hd_extras[s][k]) !=
                   (W.hd_name(back_extras[s]) if k == "name" else back_extras[s][k])]
        print("  file %d: %d/%d shared fields identical%s%s" % (
            s + 1, len(W.FIELD_NAMES) - len(diffs), len(W.FIELD_NAMES),
            ("  DIFF: " + ", ".join(diffs)) if diffs else "",
            ("  (HD-only reset: " + ", ".join(hd_only) + ")") if hd_only else ""))
    return bad


def main():
    bad = 0
    for p in sys.argv[1:]:
        print(p)
        bad += check(p)
    print("FAIL" if bad else "OK: all shared fields identical")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
