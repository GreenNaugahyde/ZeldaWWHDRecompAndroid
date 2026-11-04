# BC1 / BC3 / BC4 / BC5 blocks to and from RGBA (numpy arrays h x w x 4, uint8), rows of 4x4 blocks.
import numpy as np


def _blocks(img):
    h, w = img.shape[:2]
    bh, bw = (h + 3) // 4, (w + 3) // 4
    pad = np.zeros((bh * 4, bw * 4, img.shape[2]), np.uint8)
    pad[:h, :w] = img
    pad[:h, w:] = img[:, -1:]  # (edges repeated)
    pad[h:, :] = pad[h - 1:h, :]
    return pad.reshape(bh, 4, bw, 4, -1).transpose(0, 2, 1, 3, 4).reshape(bh * bw, 16, -1), bh, bw


def _unblocks(b, bh, bw, h, w):
    c = b.shape[-1]
    return b.reshape(bh, bw, 4, 4, c).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, c)[:h, :w]


# ---- decoding

def _565(v):
    r = (v >> 11 & 31) * 255 // 31
    g = (v >> 5 & 63) * 255 // 63
    b = (v & 31) * 255 // 31
    return np.stack([r, g, b], -1).astype(np.int32)


def _dec_color(data, n, stride, off, bc1):
    raw = np.frombuffer(data, np.uint8)[:n * stride].reshape(n, stride)[:, off:off + 8]
    c0 = raw[:, 0].astype(np.int32) | raw[:, 1].astype(np.int32) << 8
    c1 = raw[:, 2].astype(np.int32) | raw[:, 3].astype(np.int32) << 8
    bits = raw[:, 4].astype(np.uint32) | raw[:, 5].astype(np.uint32) << 8 | raw[:, 6].astype(np.uint32) << 16 | raw[:, 7].astype(np.uint32) << 24
    a, b = _565(c0), _565(c1)
    four = (c0 > c1) | (not bc1)
    p2 = np.where(four[:, None], (2 * a + b) // 3, (a + b) // 2)
    p3 = np.where(four[:, None], (a + 2 * b) // 3, 0)
    pal = np.stack([a, b, p2, p3], 1)  # n x 4 x 3
    alpha = np.full((n, 4), 255)
    if bc1:
        alpha[:, 3] = np.where(four, 255, 0)
    idx = (bits[:, None] >> (2 * np.arange(16, dtype=np.uint32))) & 3
    rgb = np.take_along_axis(pal, idx[:, :, None].astype(np.int64).repeat(3, 2), 1)
    a = np.take_along_axis(alpha, idx.astype(np.int64), 1)
    return np.concatenate([rgb, a[:, :, None]], -1).astype(np.uint8)


def _dec_alpha(data, n, stride, off):
    raw = np.frombuffer(data, np.uint8)[:n * stride].reshape(n, stride)[:, off:off + 8].astype(np.int32)
    a0, a1 = raw[:, 0], raw[:, 1]
    bits = np.zeros(n, np.uint64)
    for i in range(6):
        bits |= raw[:, 2 + i].astype(np.uint64) << np.uint64(8 * i)
    pal = np.zeros((n, 8), np.int32)
    pal[:, 0], pal[:, 1] = a0, a1
    eight = a0 > a1
    for i in range(1, 7):
        pal[:, 1 + i] = np.where(eight, ((7 - i) * a0 + i * a1) // 7, 0)
    for i in range(1, 5):
        pal[:, 1 + i] = np.where(eight, pal[:, 1 + i], ((5 - i) * a0 + i * a1) // 5)
    pal[:, 6] = np.where(eight, pal[:, 6], 0)
    pal[:, 7] = np.where(eight, pal[:, 7], 255)
    idx = (bits[:, None] >> (np.uint64(3) * np.arange(16, dtype=np.uint64))) & np.uint64(7)
    return np.take_along_axis(pal, idx.astype(np.int64), 1).astype(np.uint8)


def decode(fmt, w, h, data):
    """fmt: GX2 surface format (low 6 bits: 0x31 BC1 .. 0x35 BC5) -> RGBA"""
    f = fmt & 0x3F
    bh, bw = (h + 3) // 4, (w + 3) // 4
    n = bh * bw
    if f == 0x31:
        b = _dec_color(data, n, 8, 0, True)
    elif f == 0x33:
        b = _dec_color(data, n, 16, 8, False)
        b[:, :, 3] = _dec_alpha(data, n, 16, 0)
    elif f == 0x34:
        r = _dec_alpha(data, n, 8, 0)
        b = np.stack([r, r, r, np.full_like(r, 255)], -1)
    elif f == 0x35:
        r, g = _dec_alpha(data, n, 16, 0), _dec_alpha(data, n, 16, 8)
        b = np.stack([r, g, np.zeros_like(r), np.full_like(r, 255)], -1)
    else:
        raise ValueError('format %x' % fmt)
    return _unblocks(b, bh, bw, h, w)


# ---- encoding (endpoints along the block's main colour axis, then the nearest palette entry per texel)

def _enc_color(px, bc1_alpha):
    """px: n x 16 x 4 -> n x 8 bytes"""
    rgb = px[:, :, :3].astype(np.float32)
    n = len(px)
    mean = rgb.mean(1, keepdims=True)
    d = rgb - mean
    cov = np.einsum('nki,nkj->nij', d, d)
    axis = np.ones((n, 3), np.float32)
    for _ in range(8):  # power iteration
        axis = np.einsum('nij,nj->ni', cov, axis)
        axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-6)
    t = np.einsum('nki,ni->nk', d, axis)
    lo = mean[:, 0] + axis * t.min(1)[:, None]
    hi = mean[:, 0] + axis * t.max(1)[:, None]

    def q565(c):
        c = np.clip(c, 0, 255)
        r = np.round(c[:, 0] * 31 / 255).astype(np.int32)
        g = np.round(c[:, 1] * 63 / 255).astype(np.int32)
        b = np.round(c[:, 2] * 31 / 255).astype(np.int32)
        return r << 11 | g << 5 | b

    c0, c1 = q565(hi), q565(lo)
    has_alpha = (px[:, :, 3] < 128).any(1) if bc1_alpha else np.zeros(n, bool)
    # four colours: c0 > c1; three + transparent: c0 <= c1
    swap = np.where(has_alpha, c0 > c1, c0 < c1)
    c0, c1 = np.where(swap, c1, c0), np.where(swap, c0, c1)
    same = (c0 == c1) & ~has_alpha
    c0 = np.where(same & (c0 < 0xFFFF), c0 + 1, c0)  # (equal endpoints would mean three colours)
    a, b = _565(c0).astype(np.float32), _565(c1).astype(np.float32)
    four = ~has_alpha
    p2 = np.where(four[:, None], (2 * a + b) / 3, (a + b) / 2)
    p3 = np.where(four[:, None], (a + 2 * b) / 3, 1e9)
    pal = np.stack([a, b, p2, p3], 1)
    dist = ((rgb[:, :, None, :] - pal[:, None, :, :]) ** 2).sum(-1)
    idx = dist.argmin(2).astype(np.uint32)
    if bc1_alpha:
        idx = np.where(has_alpha[:, None] & (px[:, :, 3] < 128), 3, np.where(idx == 3, 2, idx))
    bits = (idx << (2 * np.arange(16, dtype=np.uint32))).sum(1).astype(np.uint32)
    out = np.zeros((n, 8), np.uint8)
    out[:, 0], out[:, 1], out[:, 2], out[:, 3] = c0 & 255, c0 >> 8, c1 & 255, c1 >> 8
    for i in range(4):
        out[:, 4 + i] = bits >> (8 * i) & 255
    return out


def _enc_alpha(v):
    """v: n x 16 values -> n x 8 bytes (eight-value mode)"""
    v = v.astype(np.int32)
    n = len(v)
    a0, a1 = v.max(1), v.min(1)
    a0 = np.where(a0 == a1, np.minimum(a0 + 1, 255), a0)
    a1 = np.where(a0 == a1, a0 - 1, a1)
    pal = np.stack([a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)], 1)
    idx = np.abs(v[:, :, None] - pal[:, None, :]).argmin(2).astype(np.uint64)
    bits = (idx << (np.uint64(3) * np.arange(16, dtype=np.uint64))).sum(1)
    out = np.zeros((n, 8), np.uint8)
    out[:, 0], out[:, 1] = a0, a1
    for i in range(6):
        out[:, 2 + i] = (bits >> np.uint64(8 * i)) & np.uint64(255)
    return out


def encode(fmt, img):
    """RGBA (h x w x 4) -> bytes in the GX2 BC format (BC5: red and green, BC4: red)"""
    f = fmt & 0x3F
    px, _bh, _bw = _blocks(img)
    if f == 0x31:
        return _enc_color(px, True).tobytes()
    if f == 0x33:
        return np.concatenate([_enc_alpha(px[:, :, 3]), _enc_color(px, False)], 1).tobytes()
    if f == 0x34:
        return _enc_alpha(px[:, :, 0]).tobytes()
    if f == 0x35:
        return np.concatenate([_enc_alpha(px[:, :, 0]), _enc_alpha(px[:, :, 1])], 1).tobytes()
    raise ValueError('format %x' % fmt)
