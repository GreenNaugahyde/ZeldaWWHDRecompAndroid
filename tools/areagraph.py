#!/usr/bin/env python3
"""The area graph for the shader head start by area: which areas lead into which.

Usage:
  areagraph.py GAME_DIR OUT.txt     GAME_DIR: the game folder (code/, content/, meta/)

Areas are named as at runtime (mods::current_area, runtime/src/mods/warp.cpp): the stage, on the Great
Sea "sea:<room>" (the chart square). Links come from the exits (SCLS) in each stage's and room's data,
which WWHD keeps inside the bfres of content/Common/Stage/<stage>_Stage.szs / _Room<n>.szs (the start's
sea rooms in Pack/*.pack); found by the chunk table layout as in runtime/src/android/warp_catalog.cpp.
An exit into "sea" with a room links that square; the sea squares link their 8 neighbours. Links go
both ways; the exits of the shared stages (ships, shops, test stages: GENERIC) name no place and are left out. Output: one "a b" pair per line, sorted; tools/shadermerge.py takes it as an input (*.txt)
and puts it into the template. Holds stage names only, no game data.
"""
import os
import struct
import sys
from concurrent.futures import ThreadPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shaderprep as sp  # noqa: E402

TAGS = (b'PLYR', b'SCLS', b'FILI', b'STAG', b'RTBL')
# exits from these don't name a place (shared ship / shop / test stages; warp_catalog.cpp kGenericExits)
GENERIC = {'Abship', 'Obshop', 'PShip', 'PShip2', 'PShip3', 'sea', 'sea_E', 'I_SubAN', 'ITest61', 'SubD44', 'SubD51', 'Cave06'}


def is_cc(b, p):
    return all(48 <= c < 123 for c in b[p:p + 4])


def dzx_tables(b):
    out = set()
    for tag in TAGS:
        q = b.find(tag)
        while q >= 0:
            for k in range(80):
                if q < 4 + 12 * k:
                    break
                x = q - 4 - 12 * k
                n, = struct.unpack_from('>I', b, x)
                if n <= k or n > 100 or x + 16 + 12 * n > len(b):
                    continue
                if all(is_cc(b, x + 4 + 12 * i) for i in range(n)) and struct.unpack_from('>I', b, x + 12)[0] == 4 + 12 * n:
                    out.add(x)
                    break
            q = b.find(tag, q + 1)
    return out


def exits(b):
    """(stage, room) of every SCLS entry in a bfres"""
    out = []
    for x in dzx_tables(b):
        n, = struct.unpack_from('>I', b, x)
        for i in range(n):
            tag, cnt, off = struct.unpack_from('>4sII', b, x + 4 + 12 * i)
            if tag != b'SCLS':
                continue
            off += x
            for j in range(cnt):
                e = b[off + 12 * j:off + 12 * j + 12]
                if len(e) == 12:
                    out.append((e[:8].split(b'\0')[0].decode('latin-1'), e[9]))
    return out


def stage_room(name):
    """"<stage>_Stage.szs" -> (stage, -1), "<stage>_Room<n>.szs" -> (stage, n), else None"""
    if not name.endswith('.szs'):
        return None
    base = name[:-4]
    stage, _, tail = base.rpartition('_')
    if not stage:
        return None
    if tail == 'Stage':
        return stage, -1
    if tail.startswith('Room') and tail[4:].isdigit():
        return stage, int(tail[4:])
    return None


def area(stage, room):
    return f'sea:{room}' if stage == 'sea' and 1 <= room <= 49 else stage


def scan(item):
    name, data = item
    sr = stage_room(name)
    found = []
    for path, f in sp.walk(data, name):
        if path.endswith('.bfres'):
            found += exits(f)
    return sr, found


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(1)
    common = os.path.join(sys.argv[1], 'content', 'Common')
    items = []
    for f in sorted(os.listdir(os.path.join(common, 'Stage'))):
        if stage_room(f):
            items.append((f, open(os.path.join(common, 'Stage', f), 'rb').read()))
    for p in sorted(os.listdir(os.path.join(common, 'Pack'))):
        if p.endswith('.pack'):
            for n, d in sp.sarc(open(os.path.join(common, 'Pack', p), 'rb').read()):
                if stage_room(n):
                    items.append((n, d))
    links = set()
    with ThreadPoolExecutor(8) as ex:
        for (stage, room), found in ex.map(scan, items):
            a = area(stage, room)
            if stage in GENERIC:
                continue
            for dst, droom in found:
                b = area(dst, droom)
                if b != a and b != 'sea':
                    links.add(tuple(sorted((a, b))))
    for r in range(49):
        row, col = divmod(r, 7)
        for dr in (-1, 0, 1):
            for dc in (-1, 0, 1):
                if (dr or dc) and 0 <= row + dr < 7 and 0 <= col + dc < 7:
                    links.add(tuple(sorted((f'sea:{r + 1}', f'sea:{(row + dr) * 7 + col + dc + 1}'))))
    with open(sys.argv[2], 'w') as o:
        for a, b in sorted(links):
            o.write(f'{a} {b}\n')
    areas = {x for l in links for x in l}
    print(f'{sys.argv[2]}: {len(links)} links between {len(areas)} areas ({len(items)} stage files)')


if __name__ == '__main__':
    main()
