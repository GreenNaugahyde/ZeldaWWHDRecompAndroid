# Another HD model's sword into HD Link's sword (loot swords, notes/loot.md): its mesh laid along Link's sword axis
# (hand frame of swms / swgripms: one axis for blade and hilt), cut at its cross-guard: the blade into the blade model
# (swms), cross-guard / grip / pommel into the hilt model (swgripms, also what shows in the scabbard). Its texture is
# written into both models' texture slots (same size and format as HD's, as convert.py does). Every vertex is bound
# to one joint (blade: SWMSA_jnt, hilt: SWgripMS) with the weights copied from an HD vertex bound so.
#   python3 hd_sword.py Link.bfres Pgsw.bfres bpg_ken1 OUT_Link.bfres --source-axis z --pommel -130 --guard -62
#                       [--length 1.3] [--shape 0] [--flip]
import argparse
import math
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bc  # noqa: E402
from convert import Bfres, replace_textures  # noqa: E402

# Link's sword in the hand frame (bind pose of swms + swgripms, measured): the axis in the x/y plane, its centre line,
# the pommel's and the guard's places along it, the blade's flat side across it (the guard wings' direction) and the
# thin side (z)
AXIS = np.array([1.0, 0.84, 0.0]) / math.hypot(1.0, 0.84)
ACROSS = np.array([-0.84, 1.0, 0.0]) / math.hypot(1.0, 0.84)
THIN = np.array([0.0, 0.0, 1.0])
LINE = ACROSS * -4.5 + THIN * -0.5
LINK_POMMEL, LINK_GUARD, LINK_TIP = -17.0, 16.0, 86.0


def read_shape(f, fm, si):
    """a shape's triangles (first mesh) and vertices: positions, normals, uvs (model space for rigid shapes)"""
    sname, so = f.group(f.rel(fm + 20))[si]
    vb = f.rel(fm + 16) + f.u16(so + 0x12) * 0x20
    al, bl = f.rel(vb + 0x10), f.rel(vb + 0x18)
    attrs = {f.string(f.rel(al + k * 12)): (f.u16(al + k * 12 + 6), f.u32(al + k * 12 + 8)) for k in range(f.u8(vb + 4))}
    n, st, data = f.u32(vb + 8), f.u16(bl + 0xC), f.rel(bl + 0x14)
    P = np.array([struct.unpack_from('>3f', f.d, data + i * st + attrs['_p0'][0]) for i in range(n)])
    N = np.array([struct.unpack_from('>3f', f.d, data + i * st + attrs['_n0'][0]) for i in range(n)])
    U = np.array([struct.unpack_from('>2f', f.d, data + i * st + attrs['_u0'][0]) for i in range(n)])
    mesh = f.rel(so + 0x24)
    ifmt, cnt = f.u32(mesh + 4), f.u32(mesh + 8)
    ib = f.rel(f.rel(mesh + 0x14) + 0x14)
    idx = struct.unpack_from('>%d%s' % (cnt, 'I' if ifmt == 9 else 'H'), f.d, ib)
    tris = [idx[i:i + 3] for i in range(0, cnt - 2, 3)]
    return P, N, U, tris


def texture_rgba(f, name, gx2tile):
    """an HD texture's level 0 as RGBA (untiled + decoded)"""
    o = dict(f.group(f.section(1)))[name]
    w, h, fmt = f.u32(o + 8), f.u32(o + 0xC), f.u32(o + 0x18)
    sz, tm, sw = f.u32(o + 0x24), f.u32(o + 0x34), f.u32(o + 0x38)
    with tempfile.TemporaryDirectory() as t:
        open(os.path.join(t, 'img'), 'wb').write(bytes(f.d[f.rel(o + 0xB0):f.rel(o + 0xB0) + sz]))
        open(os.path.join(t, 'mip'), 'wb').write(b'')
        subprocess.run([gx2tile, 'untile', hex(fmt), str(w), str(h), '1', str(tm), hex(sw), os.path.join(t, 'img'),
                        os.path.join(t, 'mip'), os.path.join(t, 'lin')], capture_output=True, check=True)
        px = bc.decode(fmt, w, h, open(os.path.join(t, 'lin'), 'rb').read())
    return w, h, px


def write_shape(f, fm, si, P, N, U, tris, ref_vertex, log):
    """the shape's vertex buffer and its first mesh's index buffer replaced (appended); attributes other than
    position / normal / uv copied from HD vertex `ref_vertex` (its joint binding)"""
    sname, so = f.group(f.rel(fm + 20))[si]
    vb = f.rel(fm + 16) + f.u16(so + 0x12) * 0x20
    al, bl = f.rel(vb + 0x10), f.rel(vb + 0x18)
    attrs = {f.string(f.rel(al + k * 12)): (f.u16(al + k * 12 + 6), f.u32(al + k * 12 + 8)) for k in range(f.u8(vb + 4))}
    st, data = f.u16(bl + 0xC), f.rel(bl + 0x14)
    ref = bytes(f.d[data + ref_vertex * st:data + (ref_vertex + 1) * st])
    out = bytearray(ref * max(len(P), 1))
    for i in range(len(P)):
        o = i * st
        struct.pack_into('>3f', out, o + attrs['_p0'][0], *P[i])
        struct.pack_into('>3f', out, o + attrs['_n0'][0], *N[i])
        struct.pack_into('>2f', out, o + attrs['_u0'][0], *U[i])
    at = f.append(bytes(out), 0x100)
    f.put32(vb + 8, max(len(P), 1))
    f.put32(bl + 4, len(out))
    f.set_rel(bl + 0x14, at)
    mesh = f.rel(so + 0x24)
    ifmt = f.u32(mesh + 4)
    flat = [i for t in tris for i in t] or [0, 0, 0]
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
    if len(P):
        lo, hi = P.min(0), P.max(0)
        c, e = (lo + hi) / 2, (hi - lo) / 2
        f.putf(so + 0x1C, float(np.linalg.norm(e)))
        if f.u16(so + 0x1A) == 0 and f.rel(so + 0x30):
            for k in range(1 + f.u16(mesh + 0xC) + 1):
                f.putf(f.rel(so + 0x30) + 24 * k, *(list(c) + list(e)))
    log('  %s: %d vertices, %d triangles' % (sname, len(P), len(tris)))


def ref_vertex(f, fm, si, want_i0):
    """the first vertex of the shape whose _i0 bytes start with want_i0 and whose first weight is 1 (if any)"""
    sname, so = f.group(f.rel(fm + 20))[si]
    vb = f.rel(fm + 16) + f.u16(so + 0x12) * 0x20
    al, bl = f.rel(vb + 0x10), f.rel(vb + 0x18)
    attrs = {f.string(f.rel(al + k * 12)): (f.u16(al + k * 12 + 6), f.u32(al + k * 12 + 8)) for k in range(f.u8(vb + 4))}
    n, st, data = f.u32(vb + 8), f.u16(bl + 0xC), f.rel(bl + 0x14)
    for i in range(n):
        o = data + i * st
        if '_i0' in attrs and f.d[o + attrs['_i0'][0]] != want_i0:
            continue
        if '_w0' in attrs and struct.unpack_from('>f', f.d, o + attrs['_w0'][0])[0] < 0.999:
            continue
        return i
    return 0


def tint(f, fm, material, rgb, log):
    """mat_color0 / mat_color1 of one material (what its shader multiplies the texture's intensity with)"""
    for mn, mo in f.group(f.rel(fm + 24)):
        if mn != material:
            continue
        pl, pd = f.rel(mo + 0x34), f.rel(mo + 0x3C)
        for k in range(f.u16(mo + 0x12)):
            e = pl + k * 0x14
            if f.string(f.rel(e + 0x10)) in ('mat_color0', 'mat_color1') and f.u8(e) == 0xF:
                f.putf(pd + f.u16(e + 2), *(rgb[:3] + [1.0]))
        log('  material %s: colour %s' % (mn, rgb))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('link')
    ap.add_argument('source')
    ap.add_argument('model')
    ap.add_argument('out')
    ap.add_argument('--shape', type=int, default=0)
    ap.add_argument('--source-axis', default='z', help='the source sword\'s long axis: x, y or z')
    ap.add_argument('--pommel', type=float, required=True, help='pommel end along that axis (source units)')
    ap.add_argument('--guard', type=float, required=True, help='where blade and cross-guard meet along that axis')
    ap.add_argument('--length', type=float, default=1.3, help='overall length relative to the Master Sword')
    ap.add_argument('--flip', action='store_true', help='turn the blade over (its flat side the other way)')
    ap.add_argument('--texture', default=None, help='the source texture (default: its first one)')
    ap.add_argument('--tint', default=None, help='r,g,b for the blade and hilt materials (mat_color0/1; HD 0.612 grey)')
    ap.add_argument('--gx2tile', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), 'gx2tile'))
    a = ap.parse_args()
    f = Bfres(open(a.link, 'rb').read())
    src = Bfres(open(a.source, 'rb').read())
    sfm = dict(src.group(src.section(0)))[a.model]
    P, N, U, tris = read_shape(src, sfm, a.shape)
    k = 'xyz'.index(a.source_axis)
    other = [i for i in range(3) if i != k]
    # the source's long axis, its wide side (the larger spread across) and thin side
    spread = [np.ptp(P[:, i]) for i in other]
    wide, thin = (other[0], other[1]) if spread[0] >= spread[1] else (other[1], other[0])
    tip = P[:, k].max() if abs(P[:, k].max() - a.pommel) > abs(P[:, k].min() - a.pommel) else P[:, k].min()
    sign = 1.0 if tip > a.pommel else -1.0
    src_len = abs(tip - a.pommel)
    s = a.length * (LINK_TIP - LINK_POMMEL) / src_len
    flip = -1.0 if a.flip else 1.0
    # the source's centre line across: the grip's middle (between pommel and guard; a lopsided blade would push it off
    # the hand)
    grip = ((P[:, k] - a.pommel) * sign >= 0) & ((P[:, k] - a.pommel) * sign <= (a.guard - a.pommel) * sign * 0.7)
    G = P[grip] if grip.any() else P
    cw = (G[:, wide].min() + G[:, wide].max()) / 2
    ct = (G[:, thin].min() + G[:, thin].max()) / 2

    def place(p):
        along = (p[k] - a.pommel) * sign * s + LINK_POMMEL
        return LINE + AXIS * along + ACROSS * ((p[wide] - cw) * s * flip) + THIN * ((p[thin] - ct) * s)

    def turn(n):
        v = AXIS * n[k] * sign + ACROSS * n[wide] * flip + THIN * n[thin]
        return v / (np.linalg.norm(v) or 1.0)
    # (a mirror flips the winding: one of sign / flip negative keeps it, both or none... checked by the determinant)
    basis = np.array([[0.0] * 3] * 3)
    basis[:, k], basis[:, wide], basis[:, thin] = AXIS * sign, ACROSS * flip, THIN
    mirrored = np.linalg.det(basis) < 0
    Pm = np.array([place(p) for p in P])
    Nm = np.array([turn(n) for n in N])
    models = dict(f.group(f.section(0)))
    # the grip onto the hand: the middle of HD's hilt where the hand holds it (along -6..6) and the new one's there
    def hand_middle(Q):
        near = np.abs(Q @ AXIS) < 6
        Q = Q[near] if near.sum() >= 3 else Q
        return np.array([(Q @ ACROSS).min() + (Q @ ACROSS).max(), (Q @ THIN).min() + (Q @ THIN).max()]) / 2
    hd = np.vstack([read_shape(f, models['swgripms'], si)[0] for si in range(len(f.group(f.rel(models['swgripms'] + 20))))])
    hilt = np.array([((P[i][k] - a.pommel) * sign) < (a.guard - a.pommel) * sign for i in range(len(P))])
    want, have = hand_middle(hd), hand_middle(Pm[hilt])
    Pm = Pm + ACROSS * (want[0] - have[0]) + THIN * (want[1] - have[1])
    print('grip moved onto the hand by %.1f across, %.1f thin' % (want[0] - have[0], want[1] - have[1]))
    guard_link = (a.guard - a.pommel) * sign * s + LINK_POMMEL
    print('source %s/%s: %d vertices, %d triangles, length %.1f -> %.1f (scale %.3f), guard at %.1f%s' % (
        a.source, a.model, len(P), len(tris), src_len, src_len * s, s, guard_link, ', mirrored' if mirrored else ''))
    # cut at the guard: each triangle by its middle
    parts = {'blade': [], 'hilt': []}
    for t in tris:
        mid = sum((P[i][k] - a.pommel) * sign for i in t) / 3
        parts['blade' if mid >= (a.guard - a.pommel) * sign else 'hilt'].append(t[::-1] if mirrored else t)
    models = dict(f.group(f.section(0)))

    def compact(ts):
        used = sorted({i for t in ts for i in t})
        m = {o: n for n, o in enumerate(used)}
        return Pm[used], Nm[used], U[used], [[m[i] for i in t] for t in ts]
    # blade -> swms shape 0 (material A, its texture SWMSA), its other shapes emptied; bound to SWMSA_jnt
    fm = models['swms']
    bp, bn, bu, bt = compact(parts['blade'])
    write_shape(f, fm, 0, bp, bn, bu, bt, ref_vertex(f, fm, 0, 0), print)
    for si in range(1, len(f.group(f.rel(fm + 20)))):
        write_shape(f, fm, si, np.zeros((0, 3)), np.zeros((0, 3)), np.zeros((0, 2)), [], 0, print)
    # hilt -> swgripms Polygon0 (material A: SWgripAMSCI4), the other shape emptied; bound to SWgripMS
    fm = models['swgripms']
    shapes = [n for n, _ in f.group(f.rel(fm + 20))]
    main_i = shapes.index('Polygon0')
    hp, hn, hu, ht = compact(parts['hilt'])
    write_shape(f, fm, main_i, hp, hn, hu, ht, ref_vertex(f, fm, main_i, 0), print)
    for si in range(len(shapes)):
        if si != main_i:
            write_shape(f, fm, si, np.zeros((0, 3)), np.zeros((0, 3)), np.zeros((0, 2)), [], 0, print)
    # the texture into both slots
    tname = a.texture or src.group(src.section(1))[0][0]
    w, h, px = texture_rgba(src, tname, a.gx2tile)
    tex = {n: (None, w, h, np.asarray(px, np.uint8).reshape(-1).tolist(), None) for n in ('SWMSA', 'SWgripAMSCI4')}
    replace_textures(f, tex, set(tex), 'plain', a.gx2tile, print)
    if a.tint:
        rgb = [float(x) for x in a.tint.split(',')]
        for model, mat in (('swms', 'SwMSAMAT'), ('swgripms', 'SwGripMSAMAT')):
            tint(f, models[model], mat, rgb, print)
    open(a.out, 'wb').write(f.d)
    print('written', a.out, len(f.d))


if __name__ == '__main__':
    main()
