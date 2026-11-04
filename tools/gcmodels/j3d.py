# J3D models (GameCube .bdl/.bmd) and RARC archives: joints, envelopes, vertices, shapes, materials' textures.
#   python3 j3d.py Link.arc                  the archive's files, each model's chunks and joints
#   python3 j3d.py Link.arc cl out.obj       one model in its bind pose as OBJ (shapes as groups), for checking
import math
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nintendo as nin


def rarc(d):
    if d[:4] == b'Yaz0':
        d = nin.yaz0(d)
    assert d[:4] == b'RARC'
    u32 = lambda o: struct.unpack('>I', d[o:o + 4])[0]
    u16 = lambda o: struct.unpack('>H', d[o:o + 2])[0]
    dataoff = u32(0xC) + 0x20
    info = 0x20
    nfiles, fileoff, stroff = u32(info + 8), u32(info + 0xC) + 0x20, u32(info + 0x14) + 0x20
    out = {}
    for i in range(nfiles):
        e = fileoff + i * 20
        tn, doff, size = u32(e + 4), u32(e + 8), u32(e + 12)
        if (tn >> 24) & 0x02:  # a folder
            continue
        no = tn & 0xFFFFFF
        out[d[stroff + no:d.index(b'\0', stroff + no)].decode()] = d[dataoff + doff:dataoff + doff + size]
    return out


def chunks(d):
    n = struct.unpack('>I', d[0xC:0x10])[0]
    p, r = 0x20, {}
    for _ in range(n):
        t = d[p:p + 4].decode()
        sz = struct.unpack('>I', d[p + 4:p + 8])[0]
        r[t] = d[p:p + sz]
        p += sz
    return r


def strtab(c, o):
    n = struct.unpack('>H', c[o:o + 2])[0]
    r = []
    for i in range(n):
        so = struct.unpack('>H', c[o + 4 + i * 4 + 2:o + 4 + i * 4 + 4])[0]
        r.append(c[o + so:c.index(b'\0', o + so)].decode('latin1'))
    return r


# ---- matrices: 3x4 as lists of 3 rows of 4

def mat_mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) + (a[i][3] if j == 3 else 0) for j in range(4)] for i in range(3)]


def mat_apply(m, v, w=1.0):
    return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] + m[i][3] * w for i in range(3)]


def mat_srt(s, r, t):
    # J3D: scale, then rotations X, Y, Z (degrees), then translation
    cx, sx = math.cos(math.radians(r[0])), math.sin(math.radians(r[0]))
    cy, sy = math.cos(math.radians(r[1])), math.sin(math.radians(r[1]))
    cz, sz = math.cos(math.radians(r[2])), math.sin(math.radians(r[2]))
    rot = [[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
           [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
           [-sy, sx * cy, cx * cy]]
    return [[rot[i][0] * s[0], rot[i][1] * s[1], rot[i][2] * s[2], t[i]] for i in range(3)]


IDENTITY = [[1, 0, 0, 0], [0, 1, 0, 0], [0, 0, 1, 0]]

# GX vertex attributes
PNMTXIDX, TEX0MTXIDX, POS, NRM, CLR0, CLR1, TEX0 = 0, 1, 9, 10, 11, 12, 13


class Model:
    def __init__(self, data):
        self.c = chunks(data)
        self.read_joints()
        self.read_hierarchy()
        self.read_envelopes()
        self.read_draw()
        self.read_vertices()
        self.read_shapes()
        self.read_materials()

    # ---- JNT1: joints (local SRT), INF1: their parents and which material each shape draws with
    def read_joints(self):
        c = self.c['JNT1']
        n, eo, ro, so = struct.unpack('>H', c[8:10])[0], *struct.unpack('>III', c[0xC:0x18])
        names = strtab(c, so)
        remap = struct.unpack('>%dH' % n, c[ro:ro + 2 * n])
        self.joints = []
        for i in range(n):
            e = eo + remap[i] * 0x40
            s = struct.unpack('>3f', c[e + 4:e + 0x10])
            r = [x * 180 / 32768 for x in struct.unpack('>3h', c[e + 0x10:e + 0x16])]
            t = struct.unpack('>3f', c[e + 0x18:e + 0x24])
            self.joints.append({'name': names[i], 'local': mat_srt(s, r, t), 'parent': -1})

    def read_hierarchy(self):
        c = self.c['INF1']
        p = struct.unpack('>I', c[0x14:0x18])[0]
        stack, last, mat = [], None, -1  # stack: the nodes whose children follow ('j', i) / ('m', i) / ('s', i)
        self.shape_material = {}
        while True:
            t, i = struct.unpack('>HH', c[p:p + 4])
            p += 4
            if t == 0:
                break
            if t == 1:
                stack.append(last)
            elif t == 2:
                stack.pop()
            elif t == 0x10:
                up = [n for n in stack if n and n[0] == 'j']
                self.joints[i]['parent'] = up[-1][1] if up else -1
                last = ('j', i)
            elif t == 0x11:
                mat = i
                last = ('m', i)
            elif t == 0x12:
                ms = [n for n in stack if n and n[0] == 'm']
                self.shape_material[i] = ms[-1][1] if ms else mat
                last = ('s', i)
        for j in self.joints:
            j['world'] = None
        for i in range(len(self.joints)):
            self._world(i)

    def _world(self, i):
        j = self.joints[i]
        if j['world'] is None:
            j['world'] = j['local'] if j['parent'] < 0 else mat_mul(self._world(j['parent']), j['local'])
        return j['world']

    # ---- EVP1: weighted envelopes (joints, weights) and each joint's inverse bind matrix
    def read_envelopes(self):
        self.envelopes, self.inv_bind = [], {}
        c = self.c.get('EVP1')
        if not c:
            return
        n = struct.unpack('>H', c[8:10])[0]
        co, io, wo, mo = struct.unpack('>IIII', c[0xC:0x1C])
        counts = c[co:co + n]
        p, q = io, wo
        maxj = 0
        for k in counts:
            js = struct.unpack('>%dH' % k, c[p:p + 2 * k])
            ws = struct.unpack('>%df' % k, c[q:q + 4 * k])
            p += 2 * k
            q += 4 * k
            self.envelopes.append(list(zip(js, ws)))
            maxj = max([maxj] + [j + 1 for j in js])
        for j in range(maxj):
            v = struct.unpack('>12f', c[mo + j * 48:mo + j * 48 + 48])
            self.inv_bind[j] = [list(v[0:4]), list(v[4:8]), list(v[8:12])]

    # ---- DRW1: draw matrices: a joint, or a weighted envelope
    def read_draw(self):
        c = self.c['DRW1']
        n = struct.unpack('>H', c[8:10])[0]
        wo, do = struct.unpack('>II', c[0xC:0x14])
        weighted = c[wo:wo + n]
        idx = struct.unpack('>%dH' % n, c[do:do + 2 * n])
        # each as a list of (joint, weight)
        self.draw = [self.envelopes[idx[i]] if weighted[i] else [(idx[i], 1.0)] for i in range(n)]
        self.draw_weighted = [bool(w) for w in weighted]

    # ---- VTX1: the attribute arrays
    def read_vertices(self):
        c = self.c['VTX1']
        fo = struct.unpack('>I', c[8:0xC])[0]
        offs = struct.unpack('>13I', c[0xC:0xC + 52])
        slot = {POS: 0, NRM: 1, CLR0: 3, CLR1: 4}
        for t in range(8):
            slot[TEX0 + t] = 5 + t
        self.arrays, self.formats = {}, {}
        p = fo
        while True:
            attr, cnt, typ, shift = struct.unpack('>IIIB', c[p:p + 13])
            p += 16
            if attr == 0xFF:
                break
            self.formats[attr] = (cnt, typ, shift)
            start = offs[slot[attr]]
            later = [o for o in offs if o > start] + [len(c)]
            raw = c[start:min(later)]
            self.arrays[attr] = self._decode(attr, cnt, typ, shift, raw)

    @staticmethod
    def _decode(attr, cnt, typ, shift, raw):
        if attr in (CLR0, CLR1):
            out = []
            if typ == 5:  # RGBA8
                for i in range(0, len(raw) - 3, 4):
                    out.append(tuple(raw[i:i + 4]))
            elif typ in (1, 2):  # RGB8 / RGBX8
                step = 3 if typ == 1 else 4
                for i in range(0, len(raw) - step + 1, step):
                    out.append((raw[i], raw[i + 1], raw[i + 2], 255))
            elif typ == 0:  # RGB565
                for i in range(0, len(raw) - 1, 2):
                    v = struct.unpack('>H', raw[i:i + 2])[0]
                    out.append(((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, 255))
            elif typ == 3:  # RGBA4
                for i in range(0, len(raw) - 1, 2):
                    v = struct.unpack('>H', raw[i:i + 2])[0]
                    out.append(tuple(((v >> s) & 15) * 17 for s in (12, 8, 4, 0)))
            elif typ == 4:  # RGBA6
                for i in range(0, len(raw) - 2, 3):
                    v = raw[i] << 16 | raw[i + 1] << 8 | raw[i + 2]
                    out.append(tuple(((v >> s) & 63) * 255 // 63 for s in (18, 12, 6, 0)))
            return out
        n = {POS: (2, 3), NRM: (3, 9)}.get(attr, (1, 2))[min(cnt, 1)]
        if attr == NRM and cnt == 1:
            n = 9  # NBT
        fmt, size = {0: ('B', 1), 1: ('b', 1), 2: ('H', 2), 3: ('h', 2), 4: ('f', 4)}[typ]
        scale = 1.0 if typ == 4 else 1.0 / (1 << shift)
        count = len(raw) // (size * n)
        vals = struct.unpack('>%d%s' % (count * n, fmt), raw[:count * n * size])
        return [tuple(v * scale for v in vals[i * n:i * n + n]) for i in range(count)]

    # ---- SHP1: shapes as packets of primitives, each packet with its draw matrix table
    def read_shapes(self):
        c = self.c['SHP1']
        n = struct.unpack('>H', c[8:10])[0]
        so, ro, _no, ao, mo, po, mdo, plo = struct.unpack('>8I', c[0xC:0x2C])
        remap = struct.unpack('>%dH' % n, c[ro:ro + 2 * n])
        self.shapes = []
        for si in range(n):
            e = so + remap[si] * 0x28
            mtype, npk, aoff, fmi, fpi = c[e], *struct.unpack('>HHHH', c[e + 2:e + 0xA])
            attrs = []
            p = ao + aoff
            while True:
                a, t = struct.unpack('>II', c[p:p + 8])
                p += 8
                if a == 0xFF:
                    break
                attrs.append((a, t))
            tris = []  # each vertex: dict attr -> index, plus 'mtx' -> draw matrix index
            last_table = []
            for k in range(npk):
                size, off = struct.unpack('>II', c[plo + (fpi + k) * 8:plo + (fpi + k) * 8 + 8])
                _u, cnt, first = struct.unpack('>HHI', c[mdo + (fmi + k) * 8:mdo + (fmi + k) * 8 + 8])
                table = list(struct.unpack('>%dH' % cnt, c[mo + first * 2:mo + first * 2 + cnt * 2]))
                for i, v in enumerate(table):  # 0xFFFF: as the packet before
                    if v == 0xFFFF and i < len(last_table):
                        table[i] = last_table[i]
                last_table = table
                dl = c[po + off:po + off + size]
                q = 0
                while q < len(dl):
                    op = dl[q]
                    q += 1
                    if op == 0:
                        continue
                    if op not in (0x90, 0x98, 0xA0):
                        break
                    nv = struct.unpack('>H', dl[q:q + 2])[0]
                    q += 2
                    verts = []
                    for _ in range(nv):
                        v = {}
                        for a, t in attrs:
                            if t == 1:
                                val = dl[q]
                                q += 1
                            elif t == 2:
                                val = dl[q]
                                q += 1
                            elif t == 3:
                                val = struct.unpack('>H', dl[q:q + 2])[0]
                                q += 2
                            else:
                                continue
                            v[a] = val
                        v['mtx'] = table[v[PNMTXIDX] // 3] if PNMTXIDX in v else table[0]
                        verts.append(v)
                    if op == 0x90:
                        tris += [verts[i:i + 3] for i in range(0, nv - 2, 3)]
                    elif op == 0x98:
                        for i in range(nv - 2):
                            t3 = [verts[i], verts[i + 1], verts[i + 2]]
                            if i & 1:
                                t3 = [t3[1], t3[0], t3[2]]
                            tris.append(t3)
                    else:
                        tris += [[verts[0], verts[i + 1], verts[i + 2]] for i in range(nv - 2)]
            self.shapes.append({'type': mtype, 'attrs': attrs, 'tris': tris, 'material': self.shape_material.get(si, -1)})

    # ---- MAT3: names and the textures each uses (TEX1 indices)
    def read_materials(self):
        c = self.c['MAT3']
        n = struct.unpack('>H', c[8:10])[0]
        offs = struct.unpack('>30I', c[0xC:0xC + 120])
        self.material_names = strtab(c, offs[2])
        remap = struct.unpack('>%dH' % n, c[offs[1]:offs[1] + 2 * n])
        texidx_table = offs[15]
        self.material_textures = []
        for i in range(n):
            e = offs[0] + remap[i] * 0x14C
            slots = struct.unpack('>8H', c[e + 0x84:e + 0x94])
            self.material_textures.append([struct.unpack('>H', c[texidx_table + 2 * s:texidx_table + 2 * s + 2])[0]
                                           for s in slots if s != 0xFFFF])
        t = self.c.get('TEX1')
        self.texture_names = strtab(t, struct.unpack('>I', t[0x10:0x14])[0]) if t else []

    # ---- a vertex in the bind pose (model space)
    def position(self, v):
        p = self.arrays[POS][v[POS]]
        if len(p) == 2:
            p = (p[0], p[1], 0.0)
        mi = v['mtx']
        if not self.draw_weighted[mi]:  # rigid: in its joint's space
            return mat_apply(self.joints[self.draw[mi][0][0]]['world'], p)
        out = [0.0, 0.0, 0.0]
        for j, w in self.draw[mi]:
            m = mat_mul(self.joints[j]['world'], self.inv_bind[j])
            q = mat_apply(m, p)
            for k in range(3):
                out[k] += w * q[k]
        return out


def write_obj(m, path):
    with open(path, 'w') as f:
        n = 0
        for si, s in enumerate(m.shapes):
            name = m.material_names[s['material']] if s['material'] >= 0 else str(si)
            f.write('g shape%d_%s\n' % (si, name))
            for t in s['tris']:
                for v in t:
                    f.write('v %.4f %.4f %.4f\n' % tuple(m.position(v)))
                f.write('f %d %d %d\n' % (n + 1, n + 2, n + 3))
                n += 3


if __name__ == '__main__':
    a = rarc(open(sys.argv[1], 'rb').read())
    if len(sys.argv) > 3:
        m = Model(a[sys.argv[2] + '.bdl'] if sys.argv[2] + '.bdl' in a else a[sys.argv[2] + '.bmd'])
        for si, s in enumerate(m.shapes):
            js = sorted({j for t in s['tris'] for v in t for j, _ in m.draw[v['mtx']]})
            nw = max(len(m.draw[v['mtx']]) for t in s['tris'] for v in t) if s['tris'] else 0
            mat = s['material']
            print('shape', si, m.material_names[mat] if mat >= 0 else '?', 'tris', len(s['tris']), 'max weights', nw,
                  'joints', [m.joints[j]['name'] for j in js][:6], 'tex', [m.texture_names[t] for t in m.material_textures[mat]] if mat >= 0 else [])
        write_obj(m, sys.argv[3])
    else:
        print({k: len(v) for k, v in a.items()})
        for k, v in a.items():
            if k.endswith(('.bdl', '.bmd')):
                print(k, list(chunks(v)))
