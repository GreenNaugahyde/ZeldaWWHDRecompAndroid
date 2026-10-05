#!/usr/bin/env python3
"""Convert a GameCube Wind Waker memory-card save (.gci, GZLE01 USA or GZLJ01 Japan) into a
Wind Waker HD save (cking.sav, Wii U / Cemu `save/user/` layout).

usage: gc2hd.py SAVE.gci -o OUTDIR [--drop-tingle-tuner] [--name NAME] [--quiet]
       gc2hd.py --batch SRC_DIR -o OUT_ROOT       (every .gci below SRC_DIR, one folder each)

Writes OUTDIR/cking.sav. The picture album (cking_pic*.sav) and play log are not written: the
GameCube pictures are in a different format, and HD creates both files itself.
See README.md for the field mapping.
"""
import argparse
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wwsave as W  # noqa: E402


def convert_slot(g, code, drop_tingle_tuner=False, name=None):
    """one GameCube file (packed save data) -> (HD packed save data, HD extras, notes)"""
    notes = []
    hd = bytearray(W.GAMEDATA_SIZE)
    for f in W.FIELD_NAMES:                           # field by field through both layouts
        W.put(hd, W.HD_FIELDS, f, W.get(g, W.GC_FIELDS, f))
    new = W.gc_slot_is_new(g)

    # player names: GameCube keeps them in the info block (17 bytes, Shift-JIS on the Japanese
    # disc); HD shows the UTF-16 name of its own per-file area. Japanese bytes are cleared in
    # the info block (HD's own files leave these fields empty).
    if code[3:4] == b"J":
        for f in ("info.player_name", "info.name_25", "info.name_36"):
            v = W.get(hd, W.HD_FIELDS, f)
            if any(b >= 0x80 for b in v):
                W.put(hd, W.HD_FIELDS, f, bytes(len(v)))
    gc_name = W.decode_gc_name(W.get(g, W.GC_FIELDS, "info.player_name"), code)
    if name is not None:
        hd_name = name[:8]
    elif new or not gc_name:
        hd_name = "Link"                               # HD default (027265E0)
    else:
        hd_name, why = W.hd_safe_name(gc_name)
        if why:
            notes.append(why)

    # an unused GameCube file (no name) is copied as it is: starting a new game on it
    # re-initialises the data in both games
    if not new:
        # HD marks a used file by its save counter (info +0x10, u16, incremented by every save
        # in SaveMgr 02721A68, capped at 9999); 0 shows "New Game" in the HD file select.
        # GameCube leaves this field 0 and marks a used file by the player name instead.
        cnt = W.get(hd, W.HD_FIELDS, "info.save_count")
        if cnt == b"\0\0":
            W.put(hd, W.HD_FIELDS, "info.save_count", b"\0\x01")
            notes.append("save counter 0 -> 1")
        # Tingle Tuner (GameCube, item 0x21 in inventory slot 7): HD uses the same item number and
        # slot for the Tingle Bottle, handed out by the same Tingle Island event, so by default the
        # item becomes the Tingle Bottle. --drop-tingle-tuner removes it instead.
        if W.get(hd, W.HD_FIELDS, "item.slot07") == b"\x21":
            if drop_tingle_tuner:
                W.put(hd, W.HD_FIELDS, "item.slot07", b"\xff")
                W.put(hd, W.HD_FIELDS, "get_item.slot07", b"\x00")
                sel = bytearray(W.get(hd, W.HD_FIELDS, "status_a.select_item"))
                for i, v in enumerate(sel):
                    if v == 7:
                        sel[i] = 0xFF
                W.put(hd, W.HD_FIELDS, "status_a.select_item", bytes(sel))
                notes.append("Tingle Tuner removed")
            else:
                notes.append("Tingle Tuner -> Tingle Bottle (same item number 0x21)")

    extras = W.hd_extra_defaults()
    extras["name"] = hd_name
    return bytes(hd), extras, notes


def convert_gci(raw, drop_tingle_tuner=False, name=None):
    code, slots = W.read_gci(raw)
    out_slots, out_extras, report = [], [], []
    for s, g in enumerate(slots):
        if g is None:
            raise W.GciError("file %d: both copies on the card have a bad checksum" % (s + 1))
        hd, extras, notes = convert_slot(g, code, drop_tingle_tuner, name)
        out_slots.append(hd)
        out_extras.append(extras)
        report.append((s, W.gc_slot_is_new(g), W.decode_gc_name(W.get(g, W.GC_FIELDS, "info.player_name"), code),
                       extras["name"], notes))
    return code, W.write_hd(out_slots, out_extras), report


def convert_file(path, outdir, drop_tingle_tuner=False, name=None, quiet=False):
    raw = open(path, "rb").read()
    code, sav, report = convert_gci(raw, drop_tingle_tuner, name)
    os.makedirs(outdir, exist_ok=True)
    with open(os.path.join(outdir, "cking.sav"), "wb") as f:
        f.write(sav)
    if not quiet:
        print("%s (%s) -> %s/cking.sav" % (path, code.decode(), outdir))
        for s, new, gname, hname, notes in report:
            print("  file %d: %s%s" % (s + 1, "empty" if new else "%r -> %r" % (gname, hname),
                                       ("  [" + "; ".join(notes) + "]") if notes else ""))
    return code, report


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("gci", nargs="?")
    ap.add_argument("-o", "--out", required=True, help="output folder (batch: root folder)")
    ap.add_argument("--batch", metavar="SRC_DIR", help="convert every .gci below SRC_DIR")
    ap.add_argument("--drop-tingle-tuner", action="store_true",
                    help="remove the Tingle Tuner instead of turning it into the Tingle Bottle")
    ap.add_argument("--name", help="player name for all three files (default: the GameCube name)")
    ap.add_argument("--quiet", action="store_true")
    a = ap.parse_args()
    if a.batch:
        bad = 0
        for root, _, files in sorted(os.walk(a.batch)):
            for fn in sorted(files):
                if not fn.lower().endswith(".gci"):
                    continue
                rel = os.path.relpath(os.path.join(root, fn), a.batch)
                folder = rel[:-4].replace(os.sep, "-").replace(" ", "_")
                try:
                    convert_file(os.path.join(root, fn), os.path.join(a.out, folder, "user"),
                                 a.drop_tingle_tuner, a.name, a.quiet)
                except (W.GciError, W.HdError) as e:
                    bad += 1
                    print("%s: %s" % (rel, e), file=sys.stderr)
        sys.exit(1 if bad else 0)
    if not a.gci:
        ap.error("give a .gci file or --batch DIR")
    try:
        convert_file(a.gci, a.out, a.drop_tingle_tuner, a.name, a.quiet)
    except (W.GciError, W.HdError) as e:
        sys.exit("%s: %s" % (a.gci, e))


if __name__ == "__main__":
    main()
