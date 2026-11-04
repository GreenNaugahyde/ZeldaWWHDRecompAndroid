# Upscaling the small GameCube textures to HD's sizes. Their pictures are flat colours with hard edges (cel shading),
# so an edge-keeping pixel-art scaler (Scale2x, twice) followed by a light smoothing keeps lines crisp where a plain
# resize blurs them; then a resize to the exact HD size (which may have another aspect: the UVs are 0..1 either way).
import numpy as np
from PIL import Image


def scale2x(a):
    """EPX / Scale2x on an h x w x c array"""
    p = np.pad(a, ((1, 1), (1, 1), (0, 0)), mode='edge')
    B, D, F, H, E = p[:-2, 1:-1], p[1:-1, :-2], p[1:-1, 2:], p[2:, 1:-1], a
    eq = lambda x, y: (x == y).all(-1)
    cond = ~eq(B, H) & ~eq(D, F)
    e0 = np.where((cond & eq(D, B))[..., None], D, E)
    e1 = np.where((cond & eq(B, F))[..., None], F, E)
    e2 = np.where((cond & eq(D, H))[..., None], D, E)
    e3 = np.where((cond & eq(H, F))[..., None], F, E)
    h, w, c = a.shape
    out = np.empty((h * 2, w * 2, c), a.dtype)
    out[0::2, 0::2], out[0::2, 1::2], out[1::2, 0::2], out[1::2, 1::2] = e0, e1, e2, e3
    return out


def upscale(rgba, w, h, method='scale4x'):
    """rgba: h0 x w0 x 4 uint8 -> h x w x 4"""
    im = Image.fromarray(rgba, 'RGBA')
    if method == 'plain':
        return np.array(im.resize((w, h), Image.BICUBIC))
    if method == 'lanczos':
        return np.array(im.resize((w, h), Image.LANCZOS))
    if method == 'sharp':
        return sharp(rgba, w, h)
    a = rgba
    # colours that differ only a little count as the same (the GameCube's compression left noise in flat areas)
    q = (a.astype(np.int32) + 4) // 8 * 8
    q = np.clip(q, 0, 255).astype(np.uint8)
    up = scale2x(scale2x(q)) if method.startswith('scale4x') else q
    # the original colours under the scaled shapes: each output texel takes its source texel's colour
    big = np.array(Image.fromarray(a, 'RGBA').resize((up.shape[1], up.shape[0]), Image.NEAREST))
    src = np.array(Image.fromarray(q, 'RGBA').resize((up.shape[1], up.shape[0]), Image.NEAREST))
    same = (up == src).all(-1)
    out = np.where(same[..., None], big, up)
    im = Image.fromarray(out, 'RGBA')
    im = im.resize((w, h), Image.LANCZOS if method == 'scale4x' else Image.BICUBIC)
    return np.array(im)


def _steepen(ch, scale):
    """an edge between two flat areas made as steep as about 1.5 HD texels: each texel moves towards the nearer
    of its neighbourhood's two extremes (a soft shock filter), so lines get crisp without stair steps"""
    from PIL import ImageFilter
    f = ch.astype(np.float32)
    k = max(3, int(round(scale)) | 1)
    lo = np.array(Image.fromarray(ch).filter(ImageFilter.MinFilter(k)), np.float32)
    hi = np.array(Image.fromarray(ch).filter(ImageFilter.MaxFilter(k)), np.float32)
    span = hi - lo
    mid = (hi + lo) / 2
    t = np.clip((f - mid) / np.maximum(span, 1) * 2.2 + 0.5, 0, 1)
    t = t * t * (3 - 2 * t)
    out = np.where(span > 24, lo + span * t, f)
    return np.clip(out, 0, 255).astype(np.uint8)


def sharp(rgba, w, h):
    """a smooth resize, then each channel's edges steepened (flat areas and soft shading stay as they are)"""
    scale = max(w / rgba.shape[1], h / rgba.shape[0])
    up = np.array(Image.fromarray(rgba, 'RGBA').resize((w, h), Image.LANCZOS))
    return np.stack([_steepen(up[:, :, c], scale) for c in range(4)], -1)
