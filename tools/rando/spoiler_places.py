#!/usr/bin/env python3
"""Where each of TWWHD Randomizer's locations is, for the spoiler log's map (Game > Randomizer > Spoiler log, SpoilerLog.java).

The randomizer's location data (logic/data/location_data.yaml) names, for most locations, the room or stage file and the
byte offsets it changes there: the actor entry around such an offset gives the spot (its x / y / z). The square: a sea
room's number, the sea's grid for the sea's own stage file, else the square the game progress's places
(ProgressPlaces.java's table) give the stage; locations without a file (rewards, mail, shops) take their island's square
from the name ("Windfall Island - ...") as the located ones teach it. Dungeons keep their stage (the map draws its floors).

usage: spoiler_places.py GAME_DIR OUT.tsv
  OUT: name<TAB>square (0: none)<TAB>stage ("-": the sea)<TAB>x<TAB>y<TAB>z ("nan": no spot)
"""
import collections
import math
import os
import re
import struct
import sys

import yaml

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.join(HERE, "..", "..")
sys.path.insert(0, os.path.join(REPO, "notes", "progress-map-tools"))
from lib import part, be32  # noqa: E402

RANDO = os.path.join(REPO, "runtime", "third_party", "twwhd_rando")
PLACES = os.path.join(REPO, "android", "app", "src", "main", "java", "org", "wwhdrecomp", "app", "ProgressPlaces.java")
FILE = re.compile(r"(?:Stage/|@SARC@)([A-Za-z0-9_]+?)_(Stage|Room\d+)\.szs@YAZ0@SARC@[^@]+\.bfres@BFRES@(room\.dzr|stage\.dzs)$")


def sea_square(x, z):
    col, row = math.floor((x + 350000) / 100000), math.floor((z + 350000) / 100000)
    return row * 7 + col + 1 if 0 <= col < 7 and 0 <= row < 7 else 0


def stage_squares():
    """stage -> its square, where the progress places agree on one"""
    seen = collections.defaultdict(collections.Counter)
    for m in re.finditer(r'p\([A-Z_]+, [^,]+, (\d+), "([^"]+)"', open(PLACES).read()):
        seen[m.group(2)][int(m.group(1))] += 1
    return {st: c.most_common(1)[0][0] for st, c in seen.items() if len(c) == 1 and c.most_common(1)[0][0] > 0}


def entry_at(d, off):
    """(x, y, z) of the actor entry holding byte `off` of a dzr / dzs"""
    for i in range(be32(d, 0)):
        e = 4 + 12 * i
        tag = d[e:e + 4].decode("latin1")
        n, start = be32(d, e + 4), be32(d, e + 8)
        size = 0x24 if tag.startswith(("SCO", "TGSC")) else 0x20
        if tag[:3] not in ("ACT", "SCO", "TRE", "TGO", "TGS", "DOO"):
            continue
        if start <= off < start + n * size:
            k = start + (off - start) // size * size
            return struct.unpack_from(">fff", d, k + 12)
    return None


def main():
    game, out = sys.argv[1], sys.argv[2]
    locs = yaml.safe_load(open(os.path.join(RANDO, "logic", "data", "location_data.yaml"), encoding="utf-8"))
    stsq = stage_squares()
    cache = {}
    rows, prefix = [], collections.defaultdict(collections.Counter)
    for loc in locs:
        name = loc["Names"]["English"]
        paths = {}
        if "Paths" in loc:
            paths = loc["Paths"] or {}
        elif "Path" in loc:
            paths = {loc["Path"]: loc.get("Offsets") or []}
        spot, stage, room = None, None, None
        for path, offs in paths.items():
            m = FILE.search(path)
            if not m or not offs:
                continue
            st, p, f = m.group(1), m.group(2), m.group(3)
            key = (st, p, f)
            if key not in cache:
                cache[key] = part(game, st, p, ("Stage" if p == "Stage" else p) + ".bfres", f)
            d = cache[key]
            if d is None:
                continue
            xyz = entry_at(d, offs[0])
            if xyz is None:
                continue
            spot, stage, room = xyz, st, p
            break
        square = 0
        if stage == "sea":
            square = int(room[4:]) if room and room.startswith("Room") and room != "Room0" else sea_square(spot[0], spot[2])
        elif stage is not None:
            square = stsq.get(stage, 0)
        rows.append([name, square, stage, spot])
        if square and " - " in name:
            prefix[name.split(" - ")[0]][square] += 1
    learnt = {k: c.most_common(1)[0][0] for k, c in prefix.items()}
    unplaced = []
    with open(out, "w", encoding="utf-8") as f:
        f.write("# TWWHD Randomizer locations: name, square, stage (- the sea), x, y, z (tools/rando/spoiler_places.py)\n")
        for name, square, stage, spot in rows:
            if not square and " - " in name:
                square = learnt.get(name.split(" - ")[0], 0)
            if not square:
                unplaced.append(name)
            st = "-" if stage in (None, "sea") else stage
            if stage is None:
                spot = None
            xyz = "\t".join("%.0f" % v for v in spot) if spot else "nan\tnan\tnan"
            f.write("%s\t%d\t%s\t%s\n" % (name, square, st, xyz))
    print("%d locations, %d with a spot, %d without a square:" % (len(rows), sum(1 for r in rows if r[3]), len(unplaced)))
    for n in unplaced:
        print("  " + n)


if __name__ == "__main__":
    main()
