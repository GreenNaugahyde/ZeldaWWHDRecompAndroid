#!/usr/bin/env python3
"""Print the progress stored in a Wind Waker save: HD cking.sav (or a save/user folder), or a
GameCube .gci (same fields, for comparing a conversion).

usage: hd_save_info.py PATH [--file N] [--json] [--short]

PATH: cking.sav, a folder containing it (save/user or its parent save/), or a .gci.
--short prints one line per file (used for savegames/README.md); --json for scripts/tests.
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import wwsave as W  # noqa: E402


def _u8(g, f):
    return W.get(g, T, f)[0]


def _bits(v, n):
    return [i for i in range(n) if v >> i & 1]


T = W.HD_FIELDS  # identical offsets for GC (checked in wwsave)


def slot_info(g, name):
    sa = lambda f: W.get(g, T, "status_a." + f)  # noqa: E731
    max_life, life, rupee = struct.unpack(">HHH", sa("max_life") + sa("life") + sa("rupee"))
    items = [W.get(g, T, "item.slot%02d" % i)[0] for i in range(21)]
    equip = list(sa("select_equip"))
    rec = W.get(g, T, "item_record.arrow_num")[0], W.get(g, T, "item_record.bomb_num")[0]
    mx = W.get(g, T, "item_max.arrow_max")[0], W.get(g, T, "item_max.bomb_max")[0]
    tact = _u8(g, "collect.tact")
    tri = _u8(g, "collect.triforce")
    sym = _u8(g, "collect.symbol")
    fmap = W.get(g, T, "map.fmap_bits")
    getmap = int.from_bytes(W.get(g, T, "map.get_map"), "big")
    stage = W.get(g, T, "return_place.stage").split(b"\0")[0].decode("latin-1")
    room = struct.unpack(">b", W.get(g, T, "return_place.room"))[0]
    point = _u8(g, "return_place.point")
    time_of_day = struct.unpack(">f", W.get(g, T, "status_b.time"))[0]
    dungeons = {}
    for st in range(2, 9):
        v = W.get(g, T, "memory%02d.dungeon_item" % st)[0]
        if v:
            dungeons[W.STAGES[st]] = [W.DUNGEON_BITS[b] for b in _bits(v, 6)]
    sail = items[1]
    info = {
        "name": name,
        "hearts": "%g/%g" % (life / 4, max_life / 4),
        "rupees": rupee, "wallet": _u8(g, "status_a.wallet_size"),
        "magic": "%d/%d" % (_u8(g, "status_a.magic"), _u8(g, "status_a.max_magic")),
        "items": [W.ITEM_NAMES.get(v, "0x%02X" % v) for v in items if v != 0xFF],
        "sword": W.ITEM_NAMES.get(equip[0], "0x%02X" % equip[0]) if equip[0] != 0xFF else "-",
        "shield": W.ITEM_NAMES.get(equip[1], "0x%02X" % equip[1]) if equip[1] != 0xFF else "-",
        "arrows": "%d/%d" % (rec[0], mx[0]), "bombs": "%d/%d" % (rec[1], mx[1]),
        "songs": [W.SONGS[i] for i in _bits(tact, 6)],
        "triforce_shards": len(_bits(tri, 8)),
        "pearls": [W.PEARLS[i] for i in _bits(sym, 3)],
        "sail": {0x78: "Sail", 0x77: "Swift Sail"}.get(sail, None if sail == 0xFF else "0x%02X" % sail),
        # map bits (dSv_player_map_c): fmap bit 0 = square reached (onSaveArriveGrid);
        # complete map = square charted on the sea chart (probably; 49 on a finished file)
        "islands_visited": sum(1 for v in fmap if v & 1),
        "islands_charted": bin(int.from_bytes(W.get(g, T, "map.complete_map"), "big")).count("1"),
        "charts_owned": bin(getmap).count("1"),
        "dungeons": dungeons,
        "stage": "%s room %d point %d" % (stage, room, point),
        "time_of_day": round(time_of_day, 1),
        "deaths": struct.unpack(">H", W.get(g, T, "info.death_count"))[0],
        "new_game_plus": _u8(g, "info.clear_count"),
        "save_count": struct.unpack(">H", W.get(g, T, "info.save_count"))[0],
    }
    return info


def load(path):
    """returns (kind, [(slot data, name, is_empty_guess)])"""
    if os.path.isdir(path):
        for cand in (os.path.join(path, "cking.sav"), os.path.join(path, "user", "cking.sav")):
            if os.path.exists(cand):
                path = cand
                break
    raw = open(path, "rb").read()
    if path.lower().endswith(".gci"):
        code, slots = W.read_gci(raw, allow=(b"GZLE01", b"GZLJ01", b"GZLP01"))
        out = []
        for g in slots:
            if g is None:
                out.append((None, "(bad checksum)", True))
                continue
            nm = W.decode_gc_name(W.get(g, W.GC_FIELDS, "info.player_name"), code)
            out.append((g, nm, W.gc_slot_is_new(g)))
        return "GameCube %s" % code.decode(), out
    slots, extras, _ = W.read_hd(raw)
    out = []
    for g, ex in zip(slots, extras):
        # the HD file select shows "New Game" for a file whose save counter (info +0x10) is 0
        empty = W.get(g, T, "info.save_count") == b"\0\0"
        out.append((g, W.hd_name(ex), empty))
    return "HD cking.sav", out


def short(i):
    return ("%s: %s hearts, %d rupees, %d items, %d songs, %d/8 shards, pearls %d, sail %s, "
            "%d/49 islands charted, %s" % (i["name"], i["hearts"].split("/")[1], i["rupees"], len(i["items"]),
                                len(i["songs"]), i["triforce_shards"], len(i["pearls"]),
                                i["sail"] or "-", i["islands_charted"], i["stage"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("path")
    ap.add_argument("--file", type=int, choices=[1, 2, 3])
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--short", action="store_true")
    a = ap.parse_args()
    try:
        kind, slots = load(a.path)
    except (W.GciError, W.HdError, OSError) as e:
        sys.exit("%s: %s" % (a.path, e))
    res = []
    for s, (g, name, empty) in enumerate(slots):
        if a.file and a.file != s + 1:
            continue
        res.append({"file": s + 1, "empty": empty, **(slot_info(g, name) if g is not None else {})})
    if a.json:
        print(json.dumps({"kind": kind, "files": res}, indent=1, ensure_ascii=False))
        return
    if not a.short:
        print(kind, a.path)
    for r in res:
        if a.short:
            print("file %d: %s" % (r["file"], "empty" if r["empty"] else short(r)))
            continue
        print("\nfile %d%s" % (r["file"], "  (empty / new file)" if r["empty"] else ""))
        if r["empty"]:
            continue
        for k, v in r.items():
            if k in ("file", "empty"):
                continue
            if isinstance(v, list):
                v = ", ".join(v) if v else "-"
            elif isinstance(v, dict):
                v = "; ".join("%s: %s" % (d, ", ".join(b)) for d, b in v.items()) or "-"
            print("  %-20s %s" % (k, v))


if __name__ == "__main__":
    main()
