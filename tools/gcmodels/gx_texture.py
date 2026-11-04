# GameCube textures (BTI headers as in a J3D TEX1 chunk) decoded to RGBA rows.
#   python3 gx_texture.py Link.arc cl <out folder>     a model's textures as PNG (needs Pillow)
import os
import struct
import sys

I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, C4, C8, C14X2, CMPR = 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 14
BLOCK = {I4: (8, 8, 4), I8: (8, 4, 8), IA4: (8, 4, 8), IA8: (4, 4, 16), RGB565: (4, 4, 16), RGB5A3: (4, 4, 16),
         RGBA8: (4, 4, 32), C4: (8, 8, 4), C8: (8, 4, 8), C14X2: (4, 4, 16), CMPR: (8, 8, 4)}
NAMES = {I4: 'I4', I8: 'I8', IA4: 'IA4', IA8: 'IA8', RGB565: 'RGB565', RGB5A3: 'RGB5A3', RGBA8: 'RGBA8', C4: 'C4',
         C8: 'C8', C14X2: 'C14X2', CMPR: 'CMPR'}


def rgb565(v):
    return ((v >> 11) * 255 // 31, (v >> 5 & 63) * 255 // 63, (v & 31) * 255 // 31, 255)


def rgb5a3(v):
    if v & 0x8000:
        return ((v >> 10 & 31) * 255 // 31, (v >> 5 & 31) * 255 // 31, (v & 31) * 255 // 31, 255)
    return ((v >> 8 & 15) * 17, (v >> 4 & 15) * 17, (v & 15) * 17, (v >> 12 & 7) * 255 // 7)


def ia8(v):
    return (v & 255, v & 255, v & 255, v >> 8)


def size(fmt, w, h):
    bw, bh, bits = BLOCK[fmt]
    return ((w + bw - 1) // bw) * ((h + bh - 1) // bh) * bw * bh * bits // 8


def decode(fmt, w, h, data, palette=None):
    """RGBA tuples, row by row (w*h)"""
    out = [(0, 0, 0, 0)] * (w * h)
    bw, bh, bits = BLOCK[fmt]
    p = 0

    def put(x, y, c):
        if x < w and y < h:
            out[y * w + x] = c

    for by in range(0, h, bh):
        for bx in range(0, w, bw):
            if fmt == CMPR:
                for sub in range(4):
                    sx, sy = bx + (sub & 1) * 4, by + (sub >> 1) * 4
                    c0, c1 = struct.unpack('>HH', data[p:p + 4])
                    bitsv = struct.unpack('>I', data[p + 4:p + 8])[0]
                    p += 8
                    a, b = rgb565(c0), rgb565(c1)
                    if c0 > c1:
                        cs = [a, b, tuple((2 * a[i] + b[i]) // 3 for i in range(3)) + (255,),
                              tuple((a[i] + 2 * b[i]) // 3 for i in range(3)) + (255,)]
                    else:
                        cs = [a, b, tuple((a[i] + b[i]) // 2 for i in range(3)) + (255,), (0, 0, 0, 0)]
                    for i in range(16):
                        put(sx + i % 4, sy + i // 4, cs[(bitsv >> (30 - 2 * i)) & 3])
                continue
            if fmt == RGBA8:
                ar = data[p:p + 32]
                gb = data[p + 32:p + 64]
                p += 64
                for i in range(16):
                    put(bx + i % 4, by + i // 4, (ar[2 * i + 1], gb[2 * i], gb[2 * i + 1], ar[2 * i]))
                continue
            for i in range(bw * bh):
                x, y = bx + i % bw, by + i // bw
                if bits == 4:
                    v = data[p + i // 2] >> (4 if i % 2 == 0 else 0) & 15
                elif bits == 8:
                    v = data[p + i]
                else:
                    v = struct.unpack('>H', data[p + 2 * i:p + 2 * i + 2])[0]
                if fmt == I4:
                    c = (v * 17,) * 3 + (v * 17,)
                elif fmt == I8:
                    c = (v, v, v, v)
                elif fmt == IA4:
                    c = ((v & 15) * 17,) * 3 + ((v >> 4) * 17,)
                elif fmt == IA8:
                    c = ia8(v)
                elif fmt == RGB565:
                    c = rgb565(v)
                elif fmt == RGB5A3:
                    c = rgb5a3(v)
                else:  # C4 / C8 / C14X2
                    c = palette[(v & 0x3FFF) % len(palette)] if palette else (v, v, v, 255)
                put(x, y, c)
            p += bw * bh * bits // 8
    return out


def bti(data):
    """a standalone .bti (as Link.arc's linktexbci4.bti): (format, width, height, rgba, mip count)"""
    fmt, w, h = data[0], *struct.unpack('>HH', data[2:6])
    palfmt, npal, paloff = data[9], *struct.unpack('>HI', data[0xA:0x10])
    mips, doff = data[0x18], struct.unpack('>I', data[0x1C:0x20])[0]
    pal = None
    if fmt in (C4, C8, C14X2):
        vals = struct.unpack('>%dH' % npal, data[paloff:paloff + 2 * npal])
        pal = [(ia8, rgb565, rgb5a3)[palfmt](v) for v in vals]
    return fmt, w, h, decode(fmt, w, h, data[doff:doff + size(fmt, w, h)], pal), mips


def textures(tex1):
    """{name: (format, width, height, rgba, mip count)} from a J3D TEX1 chunk"""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from j3d import strtab
    n = struct.unpack('>H', tex1[8:10])[0]
    ho, so = struct.unpack('>II', tex1[0xC:0x14])
    names = strtab(tex1, so)
    r = {}
    for i in range(n):
        h = ho + i * 0x20
        fmt, _alpha, w, ht = tex1[h], tex1[h + 1], *struct.unpack('>HH', tex1[h + 2:h + 6])
        palfmt, npal, paloff = tex1[h + 9], *struct.unpack('>HI', tex1[h + 0xA:h + 0x10])
        mips = tex1[h + 0x18]
        doff = struct.unpack('>I', tex1[h + 0x1C:h + 0x20])[0]
        pal = None
        if fmt in (C4, C8, C14X2):
            raw = tex1[h + paloff:h + paloff + 2 * npal]
            vals = struct.unpack('>%dH' % npal, raw)
            pal = [(ia8, rgb565, rgb5a3)[palfmt](v) for v in vals]
        data = tex1[h + doff:h + doff + size(fmt, w, ht)]
        if names[i] not in r:
            r[names[i]] = (fmt, w, ht, decode(fmt, w, ht, data, pal), mips)
    return r


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import j3d
    from PIL import Image
    a = j3d.rarc(open(sys.argv[1], 'rb').read())
    c = j3d.chunks(a[sys.argv[2] + '.bdl'])
    os.makedirs(sys.argv[3], exist_ok=True)
    for name, (fmt, w, h, px, mips) in textures(c['TEX1']).items():
        im = Image.new('RGBA', (w, h))
        im.putdata(px)
        im.save(os.path.join(sys.argv[3], name + '.png'))
        print(name, NAMES.get(fmt, fmt), w, h, 'mips', mips)
