#!/usr/bin/env python3
"""Flight paths for the shader tour's fly mode: per stage and room, the places where something is.

Usage:
  flypath.py GAME_DIR OUT.txt [STAGE ...]     GAME_DIR: the game folder (code/, content/, meta/)
                                              STAGE: only these stages (all when none is given)

The points are the positions of the actors, objects, chests, doors and spawns in each room's data and
the stage's (DZx chunks ACTR/SCOB/TRES/TGOB/TGSC/DOOR/TGDR/PLYR and their layer variants, found as in
tools/areagraph.py); a stage-wide one goes to the room with the nearest point. Points closer together
than MERGE units become one; each room's path starts at its first spawn and goes on to the nearest
point not yet visited. Output: "stage room x y z" per line, in flight order, y raised by HOVER (Link
flies above the ground). On the device it goes into the game folder as flypath.txt (mods/flytour.cpp);
it holds positions from the game files, so it stays out of the repo and the release.
"""
import math
import os
import struct
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import areagraph as ag  # noqa: E402
import shaderprep as sp  # noqa: E402

HOVER = 150.0
MERGE = 500.0
MERGE_SEA = 1500.0  # the sea's rooms: islands with many small things
NEAR_ROOM = 3000.0  # a stage-wide point further from every room's points is left out
POINTS = ('ACT', 'TRE', 'SCO', 'PLY', 'TGO', 'TGS', 'DOO', 'TGD')
LONG = ('SCO', 'TGS', 'DOO', 'TGD')  # 0x24-byte entries (with scale), the others 0x20


def points(b):
    """[(name, x, y, z, is_spawn)] of every placed thing in a bfres"""
    out = []
    for x in ag.dzx_tables(b):
        n, = struct.unpack_from('>I', b, x)
        for i in range(n):
            tag, cnt, off = struct.unpack_from('>4sII', b, x + 4 + 12 * i)
            t = tag[:3].decode('latin-1')
            if t not in POINTS:
                continue
            size = 0x24 if t in LONG else 0x20
            off += x
            for j in range(cnt):
                e = off + size * j
                if e + size > len(b):
                    break
                px, py, pz = struct.unpack_from('>3f', b, e + 0xC)
                if not all(math.isfinite(v) and abs(v) < 1e6 for v in (px, py, pz)):
                    continue
                out.append((b[e:e + 8].split(b'\0')[0].decode('latin-1'), px, py, pz, t == 'PLY'))
    return out


def scan(item):
    name, data = item
    found = []
    for path, f in sp.walk(data, name):
        if path.endswith('.bfres'):
            found += points(f)
    return ag.stage_room(name), found


def merge(pts, r):
    out = []
    for p in pts:
        if all((p[1] - q[1]) ** 2 + (p[2] - q[2]) ** 2 + (p[3] - q[3]) ** 2 > r * r for q in out):
            out.append(p)
    return out


def order(pts):
    spawns = [p for p in pts if p[4]]
    left = list(pts)
    cur = spawns[0] if spawns else left[0]
    left.remove(cur)
    path = [cur]
    while left:
        cur = min(left, key=lambda q: (q[1] - cur[1]) ** 2 + (q[2] - cur[2]) ** 2 + (q[3] - cur[3]) ** 2)
        left.remove(cur)
        path.append(cur)
    return path


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    only = set(sys.argv[3:])
    common = os.path.join(sys.argv[1], 'content', 'Common')
    items = []
    for f in sorted(os.listdir(os.path.join(common, 'Stage'))):
        sr = ag.stage_room(f)
        if sr and (not only or sr[0] in only):
            items.append((f, open(os.path.join(common, 'Stage', f), 'rb').read()))
    for p in sorted(os.listdir(os.path.join(common, 'Pack'))):
        if p.endswith('.pack'):
            for n, d in sp.sarc(open(os.path.join(common, 'Pack', p), 'rb').read()):
                sr = ag.stage_room(n)
                if sr and (not only or sr[0] in only):
                    items.append((n, d))
    rooms, stage_wide = {}, {}
    with ThreadPoolExecutor(8) as ex:
        for (stage, room), found in ex.map(scan, items):
            if room < 0:
                stage_wide.setdefault(stage, []).extend(found)
            elif found:
                rooms.setdefault((stage, room), []).extend(found)
    for stage, found in stage_wide.items():
        mine = [(k, p) for k, v in rooms.items() if k[0] == stage for p in v]
        for p in found:
            best = min(mine, key=lambda kp: (kp[1][1] - p[1]) ** 2 + (kp[1][3] - p[3]) ** 2, default=None)
            if best and (best[1][1] - p[1]) ** 2 + (best[1][3] - p[3]) ** 2 < NEAR_ROOM ** 2:
                rooms[best[0]].append(p)
    total = 0
    with open(sys.argv[2], 'w') as o:
        for (stage, room) in sorted(rooms):
            path = order(merge(rooms[(stage, room)], MERGE_SEA if stage == 'sea' else MERGE))
            total += len(path)
            for p in path:
                o.write(f'{stage} {room} {p[1]:.0f} {p[2] + HOVER:.0f} {p[3]:.0f}\n')
    print(f'{sys.argv[2]}: {total} points in {len(rooms)} rooms ({len(items)} stage files)')


if __name__ == '__main__':
    main()
