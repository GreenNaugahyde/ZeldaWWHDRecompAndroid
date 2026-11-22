#!/usr/bin/env python3
"""Seamless houses: one event list for the sea stage and its houses (event_list.dat, big-endian).
The game keeps one stage's event list; a house's actors look their events up by name when they are made (Orca:
"Ji1_StartSpeak", ...), which happens while Link is still outside. So the sea's list gets each house's appended:
the sea's entries keep their numbers, a house's entries are shifted by what comes before them (event, staff, cut and
data numbers, the float / int / string data positions, and the event flags, so no two lists share one).
Layout: a 0x40 header of (offset, count) for events (0xB0 each), staff (0x50), cuts (0x50), data (0x40), floats (4),
ints (4), string bytes (1); the sections follow one another."""
import struct

SIZE = [0xB0, 0x50, 0x50, 0x40, 4, 4, 1]

def parse(b):
    h = struct.unpack('>14I', b[:56])
    return [b[h[2 * k]:h[2 * k] + h[2 * k + 1] * SIZE[k]] for k in range(7)], [h[2 * k + 1] for k in range(7)]

def flags_max(secs, n):
    m = -1
    for i in range(n[0]):
        m = max(m, *[v for v in struct.unpack('>5i', secs[0][i * 0xB0 + 0x80:i * 0xB0 + 0x94])])
    for i in range(n[2]):
        m = max(m, *[v for v in struct.unpack('>4i', secs[2][i * 0x50 + 0x28:i * 0x50 + 0x38])])
    return m

def shifted(secs, n, base, flag0):
    ev, st, cu, da = bytearray(secs[0]), bytearray(secs[1]), bytearray(secs[2]), bytearray(secs[3])
    def add(buf, at, d, keep_neg=True):
        v = struct.unpack('>i', buf[at:at + 4])[0]
        if not (keep_neg and v < 0):
            buf[at:at + 4] = struct.pack('>i', v + d)
    for i in range(n[0]):
        o = i * 0xB0
        add(ev, o + 0x20, base[0])
        for j in range(20):
            add(ev, o + 0x2C + 4 * j, base[1])
        for j in range(5):
            add(ev, o + 0x80 + 4 * j, flag0)
    for i in range(n[1]):
        o = i * 0x50
        add(st, o + 0x24, base[1])
        add(st, o + 0x30, base[2])
    for i in range(n[2]):
        o = i * 0x50
        add(cu, o + 0x24, base[2])
        for j in range(4):
            add(cu, o + 0x28 + 4 * j, flag0)
        add(cu, o + 0x38, base[3])
        add(cu, o + 0x3C, base[2])
    for i in range(n[3]):
        o = i * 0x40
        add(da, o + 0x20, base[3])
        t = struct.unpack('>i', da[o + 0x24:o + 0x28])[0]
        add(da, o + 0x28, base[4] if t in (0, 1) else base[5] if t == 3 else base[6] if t == 4 else 0)
        add(da, o + 0x30, base[3])
    return [bytes(ev), bytes(st), bytes(cu), bytes(da), secs[4], secs[5], secs[6]]

def moved(secs, n, d):
    """the list's places moved by d (the house's offset): its events put Link and the camera at the house's own
    coordinates. Moved: a cut's 'pos' (where Link walks to), and a camera cut's Eye / Center / StartEye / StartCenter
    unless the cut names an actor they are relative to (RelActor). Other vectors (offsets) stay."""
    fd = bytearray(secs[4])
    done = set()
    for c in range(n[2]):
        e = secs[2][c * 0x50:(c + 1) * 0x50]
        items, di = [], struct.unpack('>i', e[0x38:0x3C])[0]
        while 0 <= di < n[3] and len(items) < 64:
            dd = secs[3][di * 0x40:(di + 1) * 0x40]
            items.append((dd[:32].split(b'\0')[0], struct.unpack('>i', dd[0x24:0x28])[0], struct.unpack('>i', dd[0x28:0x2C])[0]))
            di = struct.unpack('>i', dd[0x30:0x34])[0]
        relative = any(nm == b'RelActor' for nm, t, i in items)
        for nm, t, i in items:
            if t != 1 or i in done or i < 0 or (i + 3) * 4 > len(fd):
                continue
            if nm == b'pos' or (not relative and nm in (b'Eye', b'Center', b'StartEye', b'StartCenter')):
                v = struct.unpack('>3f', fd[i * 4:i * 4 + 12])
                fd[i * 4:i * 4 + 12] = struct.pack('>3f', v[0] + d[0], v[1] + d[1], v[2] + d[2])
                done.add(i)
    return secs[:4] + [bytes(fd)] + secs[5:]

def merge(sea, houses):
    """sea: the sea's event_list.dat; houses: [(the house stage's event_list.dat, its offset (dx, dy, dz))].
    Returns the merged list."""
    secs, n = parse(sea)
    out = [bytearray(s) for s in secs]
    total = list(n)
    flag0 = flags_max(secs, n) + 1
    for hb, d in houses:
        hs, hn = parse(hb)
        hs = moved(hs, hn, d)
        fmax = flags_max(hs, hn)
        f0 = flag0 if flag0 + fmax < 320 * 32 else 0
        for k, s in enumerate(shifted(hs, hn, total, f0)):
            out[k] += s
        total = [total[k] + hn[k] for k in range(7)]
        if f0:
            flag0 += fmax + 1
    head = b''
    at = 0x40
    for k in range(7):
        head += struct.pack('>II', at, total[k])
        at += len(out[k])
    head += sea[0x38:0x40]
    return head + b''.join(bytes(s) for s in out)
