#!/usr/bin/env python3
"""Convert a Wind Waker HD save (cking.sav) into a GameCube-layout .gci (USA GZLE01 by default).

usage: hd2gc.py cking.sav -o OUT.gci [--code GZLJ01] [--set-gc-name] [--template SOME.gci]

Mainly the reverse direction for the round-trip check of gc2hd.py: the shared fields are copied
field by field; HD-only data (UTF-16 names, the HD per-file area, the picture album) has no
GameCube place and is dropped. --set-gc-name writes the HD player name into the GameCube name
field when that is empty (HD leaves it empty; GameCube shows such a file as "New Game").
--template copies the banner/icon block from an existing .gci.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wwsave as W  # noqa: E402


def hd_to_gc_slots(raw, set_gc_name=False):
    slots, extras, _ = W.read_hd(raw)
    out = []
    for g, ex in zip(slots, extras):
        gc = bytearray(W.GAMEDATA_SIZE)
        for f in W.FIELD_NAMES:
            W.put(gc, W.GC_FIELDS, f, W.get(g, W.HD_FIELDS, f))
        used = W.get(g, W.HD_FIELDS, "info.save_count") != b"\0\0"   # HD "New Game" test
        if set_gc_name and used and W.get(gc, W.GC_FIELDS, "info.player_name")[0] == 0:
            name = W.hd_name(ex)
            if name and all(ord(c) < 0x80 for c in name):
                W.put(gc, W.GC_FIELDS, "info.player_name", name.encode("ascii")[:16].ljust(17, b"\0"))
        out.append(bytes(gc))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("sav")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--code", default="GZLE01", choices=["GZLE01", "GZLJ01"])
    ap.add_argument("--set-gc-name", action="store_true")
    ap.add_argument("--template")
    a = ap.parse_args()
    slots = hd_to_gc_slots(open(a.sav, "rb").read(), a.set_gc_name)
    tpl = open(a.template, "rb").read() if a.template else None
    with open(a.out, "wb") as f:
        f.write(W.write_gci(a.code.encode(), slots, template=tpl))
    print("%s -> %s" % (a.sav, a.out))


if __name__ == "__main__":
    main()
