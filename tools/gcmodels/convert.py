# A GameCube player model (Link.arc, J3D) into HD's Link.bfres: per model (cl, ...) each HD shape gets the mesh of
# the GameCube shape with the same material (ear(2) = ear_2_) and each texture the GameCube one of the same name,
# upscaled to HD's size. HD's materials, shaders, skeleton and animations stay.
#   python3 convert.py HD_Link.bfres Link.arc OUT_Link.bfres [--models cl] [--upscale sharp|lanczos|plain]
#                      [--gx2tile path] [--obj check.obj]
# New vertex and index buffers go after the file's end (aligned); texture data is replaced in place (same size,
# format, mips and tiling as HD's), so the file grows only by the meshes.
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bc  # noqa: E402
import gx_texture  # noqa: E402
import j3d  # noqa: E402
import upscale  # noqa: E402


class Bfres:
    def __init__(self, data):
        self.d = bytearray(data)

    def u8(self, o): return self.d[o]
    def u16(self, o): return struct.unpack_from('>H', self.d, o)[0]
    def s16(self, o): return struct.unpack_from('>h', self.d, o)[0]
    def u32(self, o): return struct.unpack_from('>I', self.d, o)[0]
    def f32s(self, o, n): return list(struct.unpack_from('>%df' % n, self.d, o))
    def put16(self, o, v): struct.pack_into('>H', self.d, o, v)
    def put32(self, o, v): struct.pack_into('>I', self.d, o, v)
    def putf(self, o, *v): struct.pack_into('>%df' % len(v), self.d, o, *v)

    def rel(self, o):
        v = struct.unpack_from('>i', self.d, o)[0]
        return o + v if v else 0

    def set_rel(self, o, target):
        struct.pack_into('>i', self.d, o, target - o)

    def string(self, o):
        return self.d[o:self.d.index(b'\0', o)].decode('latin1') if o else None

    def group(self, o):
        if not o:
            return []
        n = self.u32(o + 4)
        return [(self.string(self.rel(o + 8 + i * 16 + 8)), self.rel(o + 8 + i * 16 + 12)) for i in range(1, n + 1)]

    def section(self, i):
        return self.rel(0x20 + i * 4)

    def append(self, data, align=0x100):
        """data at the end, aligned; its offset"""
        pad = (-len(self.d)) % align
        self.d += b'\0' * pad
        at = len(self.d)
        self.d += data
        self.put32(0xC, len(self.d))  # file size
        return at


def euler_matrix(s, r, t):
    # NW4F EulerXYZ (radians): X first, then Y, then Z
    cx, sx, cy, sy, cz, sz = math.cos(r[0]), math.sin(r[0]), math.cos(r[1]), math.sin(r[1]), math.cos(r[2]), math.sin(r[2])
    rot = [[cy * cz, sx * sy * cz - cx * sz, cx * sy * cz + sx * sz],
           [cy * sz, sx * sy * sz + cx * cz, cx * sy * sz - sx * cz],
           [-sy, sx * cy, cx * cy]]
    return [[rot[i][0] * s[0], rot[i][1] * s[1], rot[i][2] * s[2], t[i]] for i in range(3)]


def quat_matrix(s, q, t):
    x, y, z, w = q
    rot = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
           [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
           [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
    return [[rot[i][0] * s[0], rot[i][1] * s[1], rot[i][2] * s[2], t[i]] for i in range(3)]


def mat_inv(m):
    a = np.array(m + [[0, 0, 0, 1]], np.float64)
    return np.linalg.pinv(a)[:3].tolist()  # (a joint scaled to nothing has no inverse)


def hd_skeleton(f, fm):
    sk = f.rel(fm + 12)
    bones = []
    for name, bo in f.group(f.rel(sk + 0x10)):
        flags = f.u32(bo + 0x10)
        s, r, t = f.f32s(bo + 0x14, 3), f.f32s(bo + 0x20, 4), f.f32s(bo + 0x30, 3)
        local = euler_matrix(s, r[:3], t) if flags & 0x1000 else quat_matrix(s, r, t)
        bones.append({'name': name, 'index': f.u16(bo + 4), 'parent': f.s16(bo + 6), 'smooth': f.s16(bo + 8),
                      'rigid': f.s16(bo + 0xA), 'local': local})
    bones.sort(key=lambda b: b['index'])
    for b in bones:
        b['world'] = b['local'] if b['parent'] < 0 else j3d.mat_mul(bones[b['parent']]['world'], b['local'])
    # check against the file's inverse bind matrices
    nsmooth = f.u16(sk + 0xA)
    inv = f.rel(sk + 0x1C)
    worst = 0
    if inv:
        for b in bones:
            if b['smooth'] >= 0 and b['smooth'] < nsmooth:
                m = f.f32s(inv + b['smooth'] * 48, 12)
                im = [m[0:4], m[4:8], m[8:12]]
                prod = j3d.mat_mul(b['world'], im)
                worst = max(worst, max(abs(prod[i][j] - (1 if i == j else 0)) for i in range(3) for j in range(4)))
    return bones, worst


# HD attribute formats: (struct code, count)
FORMATS = {0x811: ('f', 3), 0x813: ('f', 4), 0x80D: ('f', 2), 0x806: ('f', 1), 0x10C: ('I', 2), 0x110: ('I', 3),
           0x112: ('I', 4), 0x105: ('I', 1)}


def gc_material_name(hd_name):
    # HD ear_2_ = GameCube ear(2)
    if hd_name.endswith('_') and '_' in hd_name[:-1]:
        base, n = hd_name[:-1].rsplit('_', 1)
        if n.isdigit():
            return '%s(%s)' % (base, n)
    return hd_name


def rot_apply(m, v):
    return [m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2] for i in range(3)]


def normalize(v):
    n = math.sqrt(sum(x * x for x in v)) or 1.0
    return [x / n for x in v]


def see_through(gc, gs, gtex, casual=None):
    """the alpha (h x w) of the first texture of a GameCube shape's material when it has see-through texels"""
    if not gtex or gs['material'] < 0:
        return None
    ts = gc.material_textures[gs['material']]
    if not ts:
        return None
    name = gc.texture_names[ts[0]]
    if name not in gtex:
        return None
    _f, w, h, px, _m = gtex[name]
    a = np.array([p[3] for p in px], np.uint8).reshape(h, w)
    if casual is not None and name == 'linktexS3TC' and (casual[1], casual[2]) == (w, h):
        # (the body's mesh is shared by both clothes: see-through only where it is so in both)
        a = np.maximum(a, np.array([p[3] for p in casual[3]], np.uint8).reshape(h, w))
    return a if (a < 128).any() else None


def invisible(alpha, gc, t):
    """a triangle whose corners and middle all sample see-through texels"""
    if j3d.TEX0 not in t[0]:
        return False
    h, w = alpha.shape
    uvs = [gc.arrays[j3d.TEX0][v[j3d.TEX0]] for v in t]
    pts = uvs + [(sum(u for u, _ in uvs) / 3, sum(v for _, v in uvs) / 3)]
    for u, v in pts:
        x, y = int((u % 1.0) * w) % w, int((v % 1.0) * h) % h
        if alpha[y, x] >= 128:
            return False
    return True


def convert_model(f, fm, gc, log, gtex=None, casual=None):
    bones, worst = hd_skeleton(f, fm)
    log('skeleton: %d bones, bind check %.2g' % (len(bones), worst))
    by_name = {b['name']: b for b in bones}
    gc_world = {j['name']: j['world'] for j in gc.joints}
    gc_world_inv = {n: mat_inv(m) for n, m in gc_world.items()}
    # GameCube joint -> its HD bone (the same name; else the nearest named parent)
    def hd_bone(ji):
        while ji >= 0:
            n = gc.joints[ji]['name']
            if n in by_name:
                return by_name[n]
            ji = gc.joints[ji]['parent']
        return bones[0]

    def smooth_bone(b):
        while b['smooth'] < 0 and b['parent'] >= 0:
            b = bones[b['parent']]
        return b

    gc_by_mat = {}
    for si, s in enumerate(gc.shapes):
        if s['material'] >= 0:
            gc_by_mat.setdefault(gc.material_names[s['material']], []).append(s)
    mats = [n for n, _ in f.group(f.rel(fm + 24))]
    vbl = f.rel(fm + 16)
    for sname, so in f.group(f.rel(fm + 20)):
        mat = mats[f.u16(so + 0xE)]
        gshapes = gc_by_mat.get(gc_material_name(mat))
        if not gshapes:
            log('  %s (%s): no GameCube shape, kept' % (sname, mat))
            continue
        skin = f.u8(so + 0x16)
        shape_bone = bones[f.u16(so + 0x10)]
        vb = vbl + f.u16(so + 0x12) * 0x20
        al, bl = f.rel(vb + 0x10), f.rel(vb + 0x18)
        attrs = {f.string(f.rel(al + k * 12)): (f.u8(al + k * 12 + 4), f.u16(al + k * 12 + 6), f.u32(al + k * 12 + 8))
                 for k in range(f.u8(vb + 4))}
        nbuf = f.u8(vb + 5)
        if nbuf != 1:
            log('  %s: %d buffers, kept' % (sname, nbuf))
            continue
        stride = f.u16(bl + 0xC)
        old_data = f.rel(bl + 0x14)
        base_colour = None
        if '_c0' in attrs:
            _b, off, fmt = attrs['_c0']
            base_colour = struct.unpack_from('>4f', f.d, old_data + off)
        # the GameCube triangles, their vertices made unique
        verts, index, tris = [], {}, []
        used_bones = set()
        dropped = 0
        cut = 0
        for gs in gshapes:
            alpha = see_through(gc, gs, gtex, casual)
            for t in gs['tris']:
                if alpha is not None and invisible(alpha, gc, t):
                    cut += 1  # (cut out on the GameCube; HD's opaque shaders would draw it black)
                    continue
                tri = []
                for v in t:
                    key = (v.get(j3d.POS), v.get(j3d.NRM), v.get(j3d.TEX0), v.get(j3d.CLR0), v['mtx'])
                    if key not in index:
                        index[key] = len(verts)
                        verts.append(v)
                    tri.append(index[key])
                tris.append([tri[0], tri[2], tri[1]])  # (GameCube fronts are clockwise, HD's counter-clockwise)
        out = bytearray(stride * len(verts))
        lo, hi = [1e9] * 3, [-1e9] * 3
        for vi, v in enumerate(verts):
            p = gc.position(v)
            infl = [(j, w) for j, w in gc.draw[v['mtx']]]
            n = gc.arrays[j3d.NRM][v[j3d.NRM]][:3] if j3d.NRM in v else (0.0, 1.0, 0.0)
            # the GameCube normal into model space (weighted joints' rotations)
            if not gc.draw_weighted[v['mtx']]:
                nm = rot_apply(gc.joints[infl[0][0]]['world'], n)
            else:
                nm = [0.0, 0.0, 0.0]
                for j, w in infl:
                    m = j3d.mat_mul(gc.joints[j]['world'], gc.inv_bind[j])
                    q = rot_apply(m, n)
                    nm = [nm[k] + w * q[k] for k in range(3)]
            # onto HD's skeleton: each joint's part moved from the GameCube bind pose to HD's
            ph, nh = [0.0] * 3, [0.0] * 3
            hd_infl = {}
            for j, w in infl:
                hb = hd_bone(j)
                gname = gc.joints[j]['name']
                g2h = j3d.mat_mul(hb['world'], gc_world_inv.get(hb['name'], gc_world_inv[gname]))
                q = j3d.mat_apply(g2h, p)
                r = rot_apply(g2h, nm)
                ph = [ph[k] + w * q[k] for k in range(3)]
                nh = [nh[k] + w * r[k] for k in range(3)]
                hd_infl[hb['index']] = hd_infl.get(hb['index'], 0) + w
            nh = normalize(nh)
            if skin == 0:
                inv = mat_inv(shape_bone['world'])
                ph, nh = j3d.mat_apply(inv, ph), normalize(rot_apply(inv, nh))
            lo = [min(lo[k], ph[k]) for k in range(3)]
            hi = [max(hi[k], ph[k]) for k in range(3)]
            uv = gc.arrays[j3d.TEX0][v[j3d.TEX0]] if j3d.TEX0 in v else (0.0, 0.0)
            col = [c / 255 for c in gc.arrays[j3d.CLR0][v[j3d.CLR0]]] if j3d.CLR0 in v else base_colour
            # weights: the strongest `skin` ones, normalised, as smooth matrix indices
            ws = sorted(((w, smooth_bone(bones[b])) for b, w in hd_infl.items()), key=lambda x: -x[0])
            if skin and len(ws) > skin:
                dropped += 1
            ws = ws[:max(skin, 1)]
            tot = sum(w for w, _ in ws) or 1.0
            for w, b in ws:
                used_bones.add(b['index'])
            # _i0/_w0 the first four, _i1/_w1 a fifth (and on), _i2 the strongest bone's rigid matrix (HD's swords
            # and shields)
            n = max(skin, 1)
            sm = [max(b['smooth'], 0) for _, b in ws] + [max(ws[0][1]['smooth'], 0)] * (n - len(ws))
            wt = [w / tot for w, _ in ws] + [0.0] * (n - len(ws))
            rig = max(ws[0][1]['rigid'], 0)
            vals = {'_p0': ph, '_n0': nh, '_u0': uv, '_c0': col or (1.0, 1.0, 1.0, 1.0), '_i0': sm[:4], '_w0': wt[:4],
                    '_i1': sm[4:] or [sm[0]], '_w1': wt[4:] or [0.0], '_i2': [rig] * 4}
            for name, (_b, off, fmt) in attrs.items():
                if name not in vals or fmt not in FORMATS:
                    raise SystemExit('%s: attribute %s format %x not handled' % (sname, name, fmt))
                code, cnt = FORMATS[fmt]
                val = list(vals[name])[:cnt] + [0] * max(0, cnt - len(vals[name]))
                if name == '_p0' and cnt == 4:
                    val[3] = 1.0
                struct.pack_into('>%d%s' % (cnt, code), out, vi * stride + off, *val)
        # the vertex buffer
        at = f.append(bytes(out), 0x100)
        f.put32(vb + 8, len(verts))
        f.put32(bl + 4, len(out))
        f.set_rel(bl + 0x14, at)
        # the index buffer (the first mesh; its sub meshes become one)
        mesh = f.rel(so + 0x24)
        ifmt = f.u32(mesh + 4)
        flat = [i for t in tris for i in t]
        ib = struct.pack('>%d%s' % (len(flat), 'I' if ifmt == 9 else 'H'), *flat)
        iat = f.append(ib, 0x100)
        f.put32(mesh + 8, len(flat))
        sub = f.rel(mesh + 0x10)
        f.put32(sub, 0)
        f.put32(sub + 4, len(flat))
        for k in range(1, f.u16(mesh + 0xC)):
            f.put32(sub + 8 * k, len(flat))
            f.put32(sub + 8 * k + 4, 0)
        ibuf = f.rel(mesh + 0x14)
        f.put32(ibuf + 4, len(ib))
        f.set_rel(ibuf + 0x14, iat)
        f.put32(mesh + 0x18, 0)
        # skin bones
        if skin:
            sb = sorted(used_bones)
            f.set_rel(so + 0x28, f.append(struct.pack('>%dH' % len(sb), *sb), 4))
            f.put16(so + 0x14, len(sb))
        # bounds: centre and half size of all, radius round the centre
        c = [(lo[k] + hi[k]) / 2 for k in range(3)]
        e = [(hi[k] - lo[k]) / 2 for k in range(3)]
        f.putf(so + 0x1C, math.sqrt(sum(x * x for x in e)))
        if f.u16(so + 0x1A) == 0 and f.rel(so + 0x30):
            nb = 1 + f.u16(mesh + 0xC) + 1
            for k in range(nb):
                f.putf(f.rel(so + 0x30) + 24 * k, *(c + e))
        if cut:
            log('  %s: %d see-through triangles left out' % (sname, cut))
        log('  %s (%s): %d vertices, %d triangles, skin %d%s' % (sname, mat, len(verts), len(tris), skin,
                                                               ', %d vertices lost weights' % dropped if dropped else ''))
    return [n for n, _ in f.group(f.rel(fm + 24))]


def hd_mean_red(f, o, fmt, w, h, sz, tm, sw, gx2tile):
    """the mean red of HD's own texture (level 0) where it is opaque (BC5: green as alpha)"""
    with tempfile.TemporaryDirectory() as t:
        open(os.path.join(t, 'img'), 'wb').write(bytes(f.d[f.rel(o + 0xB0):f.rel(o + 0xB0) + sz]))
        open(os.path.join(t, 'mip'), 'wb').write(b'')
        subprocess.run([gx2tile, 'untile', hex(fmt), str(w), str(h), '1', str(tm), hex(sw), os.path.join(t, 'img'),
                        os.path.join(t, 'mip'), os.path.join(t, 'lin')], capture_output=True, check=True)
        px = bc.decode(fmt, w, h, open(os.path.join(t, 'lin'), 'rb').read())
    cover = px[:, :, 1] > 128 if (fmt & 0x3F) == 0x35 else np.ones(px.shape[:2], bool)
    return int(round(px[:, :, 0][cover].mean())) if cover.any() else 0


def set_mat_colour(f, fm, texture, colour, log):
    """mat_color0 of the materials whose first texture is `texture` (or one of its family: mayuh.N)"""
    fam = texture.split('.')[0]
    for mn, mo in f.group(f.rel(fm + 24)):
        tl = f.rel(mo + 0x28)
        first = f.string(f.rel(tl)) if f.u8(mo + 0x11) else None
        if not first or first.split('.')[0] != fam:
            continue
        pl, pd = f.rel(mo + 0x34), f.rel(mo + 0x3C)
        for k in range(f.u16(mo + 0x12)):
            e = pl + k * 0x14
            if f.string(f.rel(e + 0x10)) == 'mat_color0' and f.u8(e) == 0xF:
                f.putf(pd + f.u16(e + 2), *(colour[:3] + [1.0]))
                log('  material %s: mat_color0 %s' % (mn, [round(c, 3) for c in colour[:3]]))


def uv_regions(gc, name, w, h):
    """per material of a GameCube model: the texels its triangles cover in texture `name` (its first texture)"""
    from PIL import ImageDraw
    regions = []
    for mi, ts in enumerate(gc.material_textures):
        if not ts or gc.texture_names[ts[0]] != name:
            continue
        im = Image.new('L', (w, h), 0)
        d = ImageDraw.Draw(im)
        for s in gc.shapes:
            if s['material'] != mi:
                continue
            for t in s['tris']:
                if j3d.TEX0 in t[0]:
                    uv = [gc.arrays[j3d.TEX0][v[j3d.TEX0]] for v in t]
                    d.polygon([((u % 1.0) * w, (v % 1.0) * h) for u, v in uv], fill=255, outline=255)
        regions.append(np.array(im) > 0)
    return regions


def bleed(src, regions=(), under=None):
    """see-through texels get the colour of the nearest visible one, and become visible (HD's shaders draw
    them); inside a material's own texels first (Cloud's hair: its yellow, not the coat next to it)"""
    def fill(rgb, known, allowed):
        for _ in range(max(src.shape)):
            todo = ~known & allowed
            if not todo.any():
                break
            acc = np.zeros_like(rgb)
            cnt = np.zeros(known.shape, np.float32)
            for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)):
                k = np.roll(known & allowed, (dy, dx), (0, 1))
                acc += np.roll(rgb, (dy, dx), (0, 1)) * k[..., None]
                cnt += k
            grow = todo & (cnt > 0)
            if not grow.any():
                break
            rgb[grow] = acc[grow] / cnt[grow][:, None]
            known = known | grow
        return rgb, known

    rgb = src[:, :, :3].astype(np.float32)
    known = src[:, :, 3] >= 128
    if under is not None and under.shape == src.shape:
        # (the casual clothes' texture: where it is see-through, the hero's texture is shown; the GameCube cuts
        # those parts away in the casual clothes, HD draws them, so in the hero's colours rather than black)
        take = ~known & (under[:, :, 3] >= 128)
        rgb[take] = under[:, :, :3][take]
        known = known | take
    if not known.any():
        return src
    done = known.copy()
    out_rgb = rgb.copy()
    for r in regions:
        if (known & r).any():
            r2, k2 = fill(rgb.copy(), known.copy(), r)
            new = k2 & ~done & r
            out_rgb[new] = r2[new]
            done |= new
    r2, _ = fill(out_rgb.copy(), done.copy(), np.ones_like(done))
    out_rgb[~done] = r2[~done]
    out = src.copy()
    out[:, :, :3] = np.clip(np.round(out_rgb), 0, 255).astype(np.uint8)
    out[:, :, 3] = 255
    return out


def hd_opaque(f, o, fmt, w, h, sz, tm, sw, gx2tile):
    """HD's own texture (level 0) has no see-through texels"""
    if (fmt & 0x3F) == 0x31:
        pass  # (BC1 can be see-through too: decode)
    with tempfile.TemporaryDirectory() as t:
        open(os.path.join(t, 'img'), 'wb').write(bytes(f.d[f.rel(o + 0xB0):f.rel(o + 0xB0) + sz]))
        open(os.path.join(t, 'mip'), 'wb').write(b'')
        subprocess.run([gx2tile, 'untile', hex(fmt), str(w), str(h), '1', str(tm), hex(sw), os.path.join(t, 'img'),
                        os.path.join(t, 'mip'), os.path.join(t, 'lin')], capture_output=True, check=True)
        px = bc.decode(fmt, w, h, open(os.path.join(t, 'lin'), 'rb').read())
    return bool((px[:, :, 3] >= 128).all())


def replace_textures(f, gc, names, method, gx2tile, log, tinted=None, owner=None):
    tinted = {} if tinted is None else tinted
    tex = gc if isinstance(gc, dict) else gx_texture.textures(gc.c['TEX1'])
    hd = dict(f.group(f.section(1)))
    for name in sorted(names):
        if name not in tex or name not in hd or name == 'ZBtoonEX':
            continue
        o = hd[name]
        w, h, mips, fmt = f.u32(o + 8), f.u32(o + 0xC), f.u32(o + 0x14), f.u32(o + 0x18)
        sz, msz, tm, sw = f.u32(o + 0x24), f.u32(o + 0x2C), f.u32(o + 0x34), f.u32(o + 0x38)
        gfmt, gw, gh, px, _ = tex[name]
        src = np.array(px, np.uint8).reshape(gh, gw, 4)
        if (fmt & 0x3F) in (0x34, 0x35):  # BC4 / BC5: intensity in red (BC5: alpha in green)
            grey = gfmt in (gx_texture.I4, gx_texture.I8, gx_texture.IA4, gx_texture.IA8) or (
                gfmt in (gx_texture.C4, gx_texture.C8) and np.ptp(src[:, :, :3].astype(int), axis=2).max() < 8)
            if grey:
                red = src[:, :, 0]
            else:
                # a coloured texture (eyebrows painted in the hair's colour): HD's shader draws red as grey, times
                # the material colour (mat_color0): red full, the texture's colour into the materials using it
                red = np.full(src.shape[:2], 255, np.uint8)
                cover = src[:, :, 3] > 128
                colour = (src[:, :, :3][cover].mean(0) / 255).tolist() if cover.any() else [1.0, 1.0, 1.0]
                # (the eyebrow shader doesn't shade with the light: full colour glows in shadow; 65 % fits sun and shade)
                colour = [c * 0.65 for c in colour]
                tinted[name] = colour
            src = np.stack([red, src[:, :, 3] if (fmt & 0x3F) == 0x35 else red, np.zeros_like(red), np.full_like(red, 255)], -1)
        # (HD's own texture fully opaque: its shaders draw every texel, so see-through ones get colours)
        if (fmt & 0x3F) in (0x31, 0x32, 0x33) and (src[:, :, 3] < 128).any() and hd_opaque(f, o, fmt, w, h, sz, tm, sw, gx2tile):
            under = None
            if name == 'linktexbci4' and owner is not None and 'TEX1' in owner.c:
                hero = gx_texture.textures(owner.c['TEX1']).get('linktexS3TC')
                if hero and (hero[1], hero[2]) == (gw, gh):
                    under = np.array(hero[3], np.uint8).reshape(gh, gw, 4)
            src = bleed(src, uv_regions(owner, name if name != 'linktexbci4' else 'linktexS3TC', gw, gh) if owner else (), under)  # (HD's shaders draw them opaque: the nearest visible colour instead of black)
        big = upscale.upscale(src, w, h, method)
        linear = b''
        level = Image.fromarray(big, 'RGBA')
        for m in range(mips):
            lw, lh = max(w >> m, 1), max(h >> m, 1)
            img = np.array(level.resize((lw, lh), Image.LANCZOS)) if m else big
            linear += bc.encode(fmt, img)
        with tempfile.TemporaryDirectory() as t:
            open(os.path.join(t, 'lin'), 'wb').write(linear)
            r = subprocess.run([gx2tile, 'tile', hex(fmt), str(w), str(h), str(mips), str(tm), hex(sw),
                                os.path.join(t, 'lin'), os.path.join(t, 'img'), os.path.join(t, 'mip')],
                               capture_output=True, text=True)
            if r.returncode:
                raise SystemExit(r.stderr)
            img = open(os.path.join(t, 'img'), 'rb').read()
            mip = open(os.path.join(t, 'mip'), 'rb').read()
        if len(img) != sz or (msz and len(mip) != msz):
            raise SystemExit('%s: sizes %d/%d, HD %d/%d' % (name, len(img), len(mip), sz, msz))
        f.d[f.rel(o + 0xB0):f.rel(o + 0xB0) + sz] = img
        if msz:
            f.d[f.rel(o + 0xB4):f.rel(o + 0xB4) + msz] = mip
        log('  texture %s: %dx%d %s -> %dx%d fmt %x, %d mips' % (name, gw, gh, gx_texture.NAMES.get(gfmt), w, h, fmt, mips))


def material_textures(f, fm):
    """the textures a model's materials use, with their families (eyeh.1 .. eyeh.N: texture pattern animations)"""
    used = set()
    for _n, mo in f.group(f.rel(fm + 24)):
        tl = f.rel(mo + 0x28)
        for k in range(f.u8(mo + 0x11)):
            used.add(f.string(f.rel(tl + k * 8)))
    fam = {u.split('.')[0] for u in used}
    return used | {n for n, _ in f.group(f.section(1)) if n.split('.')[0] in fam and n.split('.')[0] != 'ZBtoonEX'}


def visible_triangles(gc):
    n = 0
    for s in gc.shapes:
        for t in s['tris']:
            # (as stored, before the joints: effect parts have joints scaled to nothing in their bind pose)
            if any(j3d.POS not in v for v in t):
                continue
            a, b, c = (np.array(gc.arrays[j3d.POS][v[j3d.POS]][:3], float) for v in t)
            if np.linalg.norm(np.cross(b - a, c - a)) > 1e-4:
                n += 1
    return n


def choose_models(f, arc, log):
    """what this pack changed, against the game's original (baseline.json, survey.py over many packs):
    textures clearly further from HD's than the original is are replaced; a part gets the pack's mesh when its
    shape changed, or when it uses a replaced texture whose layout isn't HD's (the original already far: its UVs
    belong to the GameCube texture), and then every such texture of it is replaced too (and its other users)
    returns (models, textures)"""
    import json
    import survey
    base = json.load(open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'baseline.json')))
    hd = survey.HD(f)
    geo, tex = survey.measure(hd, arc)
    models = dict(f.group(f.section(0)))
    own = {n: material_textures(f, models[n]) for n in models if n in geo}
    t0 = base['texture']
    textures = {t for t in tex if t != 'ZBtoonEX' and tex[t] > t0.get(t, 0) + 15}
    other_layout = {t for t in tex if t0.get(t, 0) > 15 and t != 'ZBtoonEX'}
    why = {}
    for n in own:
        g0 = base['geometry'].get(n, 100)
        key = n + '.bdl' if n + '.bdl' in arc else n + '.bmd'
        if visible_triangles(j3d.Model(arc[key])) <= 2 and len(hd.model_points(n)) > 8:
            why[n] = 'taken out by the pack'  # (Link's casual hair, in most packs: one flat triangle left)
        elif g0 >= 50 and geo[n] < g0 - 15:
            why[n] = 'shape %.0f%% (original %.0f%%)' % (geo[n], g0)
        elif g0 < 50 and own[n] & textures:
            # (HD remodelled it, so its shape tells nothing; a changed texture goes with the pack's own mesh)
            why[n] = 'texture %s (HD remodelled the part)' % sorted(own[n] & textures)[0]
        elif n == 'cl':
            why[n] = 'the body'
    while True:
        more = False
        for n in own:
            if n not in why and own[n] & textures & other_layout:
                why[n] = 'texture %s in a layout of its own' % sorted(own[n] & textures & other_layout)[0]
                more = True
        for n in why:
            add = (own[n] & other_layout & set(tex)) - textures
            if add:
                textures |= add
                more = True
        if not more:
            break
    for t in sorted(textures):
        log('texture %-20s %.0f (original %.0f)' % (t, tex[t], t0.get(t, 0)))
    for n in sorted(why):
        log('mesh    %-20s %s' % (n, why[n]))
    return sorted(why), textures


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('bfres')
    ap.add_argument('arc')
    ap.add_argument('out')
    ap.add_argument('--models', default='auto', help='auto (what the pack changed), all, or a list: cl,katsura')
    ap.add_argument('--upscale', default='sharp')
    ap.add_argument('--gx2tile', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gx2tile'))
    a = ap.parse_args()
    f = Bfres(open(a.bfres, 'rb').read())
    arc = j3d.rarc(open(a.arc, 'rb').read())
    models = dict(f.group(f.section(0)))
    def has(n):
        return n + '.bdl' in arc or n + '.bmd' in arc
    if a.models == 'auto':
        names, textures = choose_models(f, arc, print)
    else:
        names = [n for n in models if has(n)] if a.models == 'all' else a.models.split(',')
        textures = set().union(*[material_textures(f, models[n]) for n in names])
    # each texture from the first part (meshes first) whose GameCube model has it
    sources = names + [n for n in models if has(n) and n not in names]
    done_tex = set()
    for name in sources:
        gc = j3d.Model(arc[name + '.bdl'] if name + '.bdl' in arc else arc[name + '.bmd'])
        gtex = gx_texture.textures(gc.c['TEX1']) if 'TEX1' in gc.c else {}
        if name in names:
            print('model', name)
            casual = gx_texture.bti(arc['linktexbci4.bti']) if name == 'cl' and 'linktexbci4.bti' in arc else None
            convert_model(f, models[name], gc, print, gtex, casual)
        have = set(gtex)
        used = (textures & have) - done_tex
        if not used:
            continue
        done_tex |= used
        tinted = {}
        replace_textures(f, gc, used, a.upscale, a.gx2tile, print, tinted, gc)
        fams = set()
        for tname, colour in sorted(tinted.items()):
            if tname.split('.')[0] not in fams:
                fams.add(tname.split('.')[0])
                set_mat_colour(f, models[name], tname, colour, print)
    # textures kept as files of their own next to the models (linktexbci4.bti: the body in the casual clothes),
    # with the body's mesh
    loose = {k[:-4]: gx_texture.bti(v) for k, v in arc.items() if k == 'linktexbci4.bti'}
    if 'cl' in names and loose:
        print('loose textures', sorted(loose))
        body = j3d.Model(arc['cl.bdl'] if 'cl.bdl' in arc else arc['cl.bmd'])
        replace_textures(f, loose, set(loose) - done_tex, a.upscale, a.gx2tile, print, {}, body)
    open(a.out, 'wb').write(f.d)
    print('written', a.out, len(f.d))


if __name__ == '__main__':
    main()
