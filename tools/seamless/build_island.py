#!/usr/bin/env python3
"""Seamless houses: everything one island needs, from the player's game files.
build_island.py GAME_CONTENT HOSTROOM OUTDIR
  GAME_CONTENT  the game's content folder (Common/Stage, Common/Pack)
  HOSTROOM      the sea room of the island (Outset 44)
Finds the island's houses (the rooms its SCLS exits lead to, in stages whose save table is 11 and stage type 2), gives
them sea room numbers from 50 on and a place 150000 above the island (one slot each), and writes into OUTDIR:
  seamless_<HOST>_<STAGE>_<ROOM>.szs   each house room as sea room 50 (build_room.py)
  <stage>.env           its stage's lighting tables (build_env.py)
  <stage>.evt / .evn    its stage's event_list.dat and EVNT entries
  sea_<HOST>.evt        the sea's event list with the houses' appended (merge_events.py)
  seamless_<HOST>.txt   the runtime's table (mods/seamless_houses.cpp):
    host ROOM
    house STAGE ROOM SEAROOM save TABLE dx DX dy DY dz DZ   (one room an island exit leads to; SEAROOM 50)
    file NAME                   (its room archive, served as sea_Room50.szs while it is the house loading)
    in POINT X Y Z ANGLE        (that room's spawn POINT, moved)
    out POINT X Y Z ANGLE       (the island's spawn POINT that the room's exits lead to; also where its door is)"""
import os, struct, sys, subprocess
from arc import yaz0, sarc
from bfres import ext_files
from dzx import tables

content, host, out = sys.argv[1], int(sys.argv[2]), sys.argv[3]
here = os.path.dirname(os.path.abspath(__file__))
stage_dir = os.path.join(content, 'Common', 'Stage')
os.makedirs(out, exist_ok=True)

def pack_files():
    files = {}
    pdir = os.path.join(content, 'Common', 'Pack')
    for p in sorted(os.listdir(pdir)):
        if 'szs' in p:
            for k, v in sarc(yaz0(open(os.path.join(pdir, p), 'rb').read())).items():
                files[k] = v
    return files
PACKS = pack_files()
def stage_file(name):
    p = os.path.join(stage_dir, name)
    return open(p, 'rb').read() if os.path.exists(p) else PACKS.get(name)
def bfres_of(szs):
    return [v for k, v in sarc(yaz0(szs)).items() if k.endswith('.bfres')][0]
def dzx_chunks(b, base):
    n = struct.unpack('>I', b[base:base + 4])[0]
    for k in range(n):
        c, cnt, off = struct.unpack('>4sII', b[base + 4 + 12 * k:base + 16 + 12 * k])
        yield c.decode('latin1'), cnt, base + off
def room_dzr(name):
    b = bfres_of(stage_file(name))
    return b, ext_files(b)['room.dzr'][0]
def stage_dzs(name):
    b = bfres_of(stage_file(name + '_Stage.szs'))
    return b, ext_files(b)['stage.dzs'][0]
def plyr(b, base):
    out = {}
    for c, cnt, at in dzx_chunks(b, base):
        if c == 'PLYR':
            for i in range(cnt):
                e = b[at + 0x20 * i:at + 0x20 * (i + 1)]
                x, y, z = struct.unpack('>fff', e[12:24])
                out[e[0x1D]] = (x, y, z, struct.unpack('>h', e[0x1A:0x1C])[0])
    return out
def scls(b, base):
    out = []
    for c, cnt, at in dzx_chunks(b, base):
        if c == 'SCLS':
            for i in range(cnt):
                e = b[at + 12 * i:at + 12 * (i + 1)]
                out.append((e[:8].split(b'\0')[0].decode(), e[8], e[9]))
    return out

# the island
hb, hbase = room_dzr('sea_Room%d.szs' % host)
host_spawns = plyr(hb, hbase)
sb, sbase = bfres_of(stage_file('sea_Stage.szs')), None
sbase = ext_files(sb)['stage.dzs'][0]
cx = cz = None
for c, cnt, at in dzx_chunks(sb, sbase):
    if c == 'MULT':
        for i in range(cnt):
            x, z, ang, room, wave = struct.unpack('>ffhBB', sb[at + 12 * i:at + 12 * (i + 1)])
            if room == host:
                cx, cz = x, z
assert cx is not None, 'no MULT entry for the host room'

# the island's door targets: (stage, room) pairs its exits lead to, house stages only (save table 11, type 2)
houses = []
for name, spawn, room in scls(hb, hbase):
    if (name, room) in houses or stage_file(name + '_Stage.szs') is None:
        continue
    b, base = stage_dzs(name)
    stag = [at for c, cnt, at in dzx_chunks(b, base) if c == 'STAG'][0]
    table, kind = b[stag + 9] >> 1, (struct.unpack('>I', b[stag + 0xC:stag + 0x10])[0] >> 16) & 7
    if table != 11 or kind != 2:
        print('skipped', name, 'table', table, 'type', kind)
        continue
    houses.append((name, room))

slots = [(-45000, -30000), (-15000, -30000), (15000, -30000), (45000, -30000), (-45000, 30000), (-15000, 30000),
         (15000, 30000), (45000, 30000), (-30000, 0), (30000, 0)]
lines = ['host %d' % host]
done_stages = set()
stage_offset = {}  # one offset per stage (its rooms share coordinates; its events are moved by it)
# every house is room 50 of the sea (one house in memory at a time; the runtime hands the game the house's file when it
# opens sea_Room50.szs): sea room numbers past 51 were not drawn (the sea's own tables stop at 51)
SLOT_ROOM = 50
for hi, (name, r) in enumerate(houses):
    sea_room = SLOT_ROOM
    house_file = 'seamless_%d_%s_%d.szs' % (host, name, r)
    dx, dy, dz = stage_offset.setdefault(name, (cx + slots[hi][0], 150000.0, cz + slots[hi][1]))
    b, base = stage_dzs(name)
    table = b[[at for c, cnt, at in dzx_chunks(b, base) if c == 'STAG'][0] + 9] >> 1
    src = os.path.join(stage_dir, '%s_Room%d.szs' % (name, r))
    subprocess.run([sys.executable, os.path.join(here, 'build_room.py'), src, str(sea_room), str(dx), str(dy), str(dz),
                    os.path.join(out, house_file), str(host)], check=True, stdout=subprocess.DEVNULL)
    lines.append('house %s %d %d save %d dx %.1f dy %.1f dz %.1f' % (name, r, sea_room, table, dx, dy, dz))
    lines.append('file %s' % house_file)
    rb, rbase = room_dzr('%s_Room%d.szs' % (name, r))
    for p, (x, y, z, a) in sorted(plyr(rb, rbase).items()):
        lines.append('in %d %.1f %.1f %.1f %d' % (p, x + dx, y + dy, z + dz, a))
    for st, p, hr in scls(rb, rbase):
        if st == 'sea' and hr == host and p in host_spawns:
            x, y, z, a = host_spawns[p]
            lines.append('out %d %.1f %.1f %.1f %d' % (p, x, y, z, a))
    print(name, r, '->', house_file)
    if name in done_stages:
        continue
    done_stages.add(name)
    subprocess.run([sys.executable, os.path.join(here, 'build_env.py'), os.path.join(stage_dir, name + '_Stage.szs'),
                    os.path.join(out, name + '.env')], check=True, stdout=subprocess.DEVNULL)
    ext = ext_files(b)
    if 'event_list.dat' in ext:
        o, sz = ext['event_list.dat']
        open(os.path.join(out, name + '.evt'), 'wb').write(b[o:o + sz])
    for c, cnt, at in dzx_chunks(b, base):
        if c == 'EVNT':
            open(os.path.join(out, name + '.evn'), 'wb').write(struct.pack('>I', cnt) + b[at:at + 0x18 * cnt])
# the sea's event list with the houses' appended (merge_events.py): sea_<HOST>.evt
from merge_events import merge
se = ext_files(sb)
o, sz = se['event_list.dat']
house_lists = [(open(os.path.join(out, st + '.evt'), 'rb').read(), stage_offset[st]) for st in sorted(done_stages)
               if os.path.exists(os.path.join(out, st + '.evt'))]
open(os.path.join(out, 'sea_%d.evt' % host), 'wb').write(merge(sb[o:o + sz], house_lists))
open(os.path.join(out, 'seamless_%d.txt' % host), 'w').write('\n'.join(lines) + '\n')
