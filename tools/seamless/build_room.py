#!/usr/bin/env python3
"""Seamless houses PoC: an interior room archive turned into an extra room of another stage.
build_room.py SRC.szs ROOMNO DX DY DZ OUT.szs [HOSTROOM] [--model-only]: positions in room.dzr and the room.dzb vertices moved by (DX, DY, DZ),
the bfres renamed Room<ROOMNO>.bfres. (The model itself is placed by the MULT answer at run time.)"""
import struct, sys
from arc import yaz0, sarc
from bfres import ext_files

def move(b, off, d):
    x, y, z = struct.unpack('>fff', b[off:off + 12])
    b[off:off + 12] = struct.pack('>fff', x + d[0], y + d[1], z + d[2])

# chunk -> (entry size, position offsets)
POS = {b'ACTR': (0x20, [0xC]), b'PLYR': (0x20, [0xC]), b'SCOB': (0x24, [0xC]), b'TRES': (0x20, [0xC]),
       b'RPPN': (0x10, [4]), b'SOND': (0x1C, [8]), b'RARO': (0x14, [0]), b'LGTV': (0x1C, [0])}
for k in '0123456789ab': POS[('ACT' + k).encode()] = (0x20, [0xC]); POS[('SCO' + k).encode()] = (0x24, [0xC]); POS[('TRE' + k).encode()] = (0x20, [0xC])

# --model-only (a look-in copy: only its model is drawn, placed by the MULT answer): no actors, spawns, sounds or
# lights, and its collision moved by (DX, DY, DZ) to somewhere nobody goes
EMPTY = {b'ACTR', b'PLYR', b'SCOB', b'TRES', b'SOND', b'LGTV', b'RPAT', b'RPPN'} | {k for k in POS if k[:3] in (b'ACT', b'SCO', b'TRE')}

def empty_dzr(b, base):
    n = struct.unpack('>I', b[base:base + 4])[0]
    for k in range(n):
        at = base + 4 + 12 * k
        if bytes(b[at:at + 4]) in EMPTY:
            b[at + 4:at + 8] = struct.pack('>I', 0)
            print("  dzr", bytes(b[at:at + 4]).decode(), "emptied")

def hide_shape(b, name):
    """the bfres shape `name` draws nothing: its first mesh (FMSH, from the FSHP's offset at +0x24) gets 0 indices and
    its visibility group 0 indices. LinkRM's umi__SC_00_umi (model1) is the sea seen through its windows: put at the
    door it flooded Outset. (Renaming model1 instead crashed the game while the room loaded.)"""
    import re
    for m in re.finditer(rb'FSHP', b):
        o = m.start()
        no = o + 4 + struct.unpack('>i', b[o + 4:o + 8])[0]
        if b[no:no + len(name) + 1] != name.encode() + b'\0':
            continue
        msh = o + 0x24 + struct.unpack('>i', b[o + 0x24:o + 0x28])[0]
        vis = msh + 0x10 + struct.unpack('>i', b[msh + 0x10:msh + 0x14])[0]
        nvis = struct.unpack('>H', b[msh + 0xC:msh + 0xE])[0]
        print('  shape', name, 'hidden: %d indices, %d visibility groups' % (struct.unpack('>I', b[msh + 8:msh + 12])[0], nvis))
        b[msh + 8:msh + 12] = struct.pack('>I', 0)
        for k in range(nvis):
            b[vis + 8 * k + 4:vis + 8 * k + 8] = struct.pack('>I', 0)
        return
    raise SystemExit('shape not found: ' + name)

def patch_dzr(b, base, d):
    n = struct.unpack('>I', b[base:base + 4])[0]
    for k in range(n):
        c, cnt, off = struct.unpack('>4sII', b[base + 4 + 12 * k:base + 16 + 12 * k])
        if c in POS:
            size, offs = POS[c]
            for i in range(cnt):
                for po in offs: move(b, base + off + i * size + po, d)
            print('  dzr', c.decode(), cnt)
        else:
            print('  dzr', c.decode(), cnt, '(kept)')

def patch_dzb(b, base, d, host_room, room):
    vn, vo = struct.unpack('>II', b[base:base + 8])
    for i in range(vn): move(b, base + vo + 12 * i, d)
    print('  dzb vertices', vn)
    # the groups' room number (cBgD_Grp_t::m_room_id at +0x2A, dBgS::GetRoomId): the room Link stays in, and whose
    # exits (SCLS) its exit polygons use: the new room (the interior's own exit back out)
    gn, go = struct.unpack('>II', b[base + 0x20:base + 0x28])
    # and their info word's low byte (cBgD_Grp_t::m_info +0x30, dBgS::GetGrpRoomInfId): the room list to load
    # (dStage_roomCheck). Outset's groups carry 0x2C there
    for i in range(gn):
        at = base + go + 0x34 * i + 0x30
        info = struct.unpack('>I', b[at:at + 4])[0]
        b[at:at + 4] = struct.pack('>I', (info & ~0xFF) | host_room)
    print('  dzb groups: room list', host_room)
    for i in range(gn):
        at = base + go + 0x34 * i + 0x2A
        if struct.unpack('>H', b[at:at + 2])[0] != 0xFFFF:
            b[at:at + 2] = struct.pack('>H', room)
            print('  dzb group', i, 'room ->', room)

def sarc_hash(name, key=0x65):
    h = 0
    for ch in name.encode(): h = (h * key + ch) & 0xFFFFFFFF
    return h

def write_sarc(files, align=0x2000):
    names = sorted(files, key=sarc_hash)
    nt = b''; ntoff = {}
    for n in names:
        ntoff[n] = len(nt); e = n.encode() + b'\0'; nt += e + b'\0' * (-len(e) % 4)
    hdr_len = 0x14 + 0xC + 16 * len(names) + 8 + len(nt)
    data_start = (hdr_len + align - 1) // align * align
    data = b''; fat = b''
    for n in names:
        data += b'\0' * (-len(data) % align)
        s = len(data); data += files[n]
        fat += struct.pack('>IIII', sarc_hash(n), 0x01000000 | (ntoff[n] // 4), s, len(data))
    total = data_start + len(data)
    out = struct.pack('>4sHHIII', b'SARC', 0x14, 0xFEFF, total, data_start, 0x01000000)
    out += struct.pack('>4sHHI', b'SFAT', 0xC, len(names), 0x65) + fat + struct.pack('>4sHH', b'SFNT', 8, 0) + nt
    return out + b'\0' * (data_start - len(out)) + data

def yaz0_store(d):  # valid Yaz0 without compression (every block literal)
    out = bytearray(b'Yaz0' + struct.pack('>I', len(d)) + b'\0' * 8)
    for i in range(0, len(d), 8):
        out.append(0xFF); out += d[i:i + 8]
    return bytes(out)

src, room, out = sys.argv[1], int(sys.argv[2]), sys.argv[6]
d = tuple(float(v) for v in sys.argv[3:6])
files = sarc(yaz0(open(src, 'rb').read()))
old = [n for n in files if n.endswith('.bfres')][0]
b = bytearray(files.pop(old))
ext = ext_files(b)
model_only = '--model-only' in sys.argv
if model_only:
    sys.argv.remove('--model-only')
    empty_dzr(b, ext['room.dzr'][0])
    hide_shape(b, 'umi__SC_00_umi')
    hide_shape(b, 'light__light_m')  # model3: the bright doorway seen from inside (from outside: a yellow frame)
else:
    patch_dzr(b, ext['room.dzr'][0], d)
patch_dzb(b, ext['room.dzb'][0], d, int(sys.argv[7]) if len(sys.argv) > 7 else 44, room)
files['Room%d.bfres' % room] = bytes(b)
open(out, 'wb').write(yaz0_store(write_sarc(files)))
print(old, '->', 'Room%d.bfres' % room, out)
