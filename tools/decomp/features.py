"""Content features of PowerPC functions, computed the same way for the GameCube objects (split by
the decompilation's build into build/GZLE01/**/obj/*.o, with names) and for WWHD (cking.rpx).

Features are multisets of tokens that survive a change of compiler:
  F:<value>   float constants loaded (lfs/lfd from a relocated address)
  S:<text>    strings referenced
  I:<imm>     integer immediates (li/addi/cmpwi/ori...), small ones excluded
  O:<off>     load/store displacements >= 0x40 (structure field offsets)
  plus counts: calls, instructions.
"""
import os
import struct
from collections import Counter

from decomp_index import float_key

# ---------------------------------------------------------------- instruction tokens


def insn_tokens(words, reloc_sites=()):
    """immediates and field offsets of a function body; reloc_sites: word indices whose immediate is
    an address (skipped)"""
    t = Counter()
    for i, w in enumerate(words):
        if i in reloc_sites:
            continue
        op = w >> 26
        simm = w & 0xFFFF
        simm = simm - 0x10000 if simm & 0x8000 else simm
        if op in (14, 15) and ((w >> 16) & 31) == 0 and op == 14:  # li
            if abs(simm) >= 8:
                t["I:%d" % simm] += 1
        elif op in (10, 11):  # cmplwi / cmpwi
            if abs(simm) >= 8:
                t["I:%d" % (w & 0xFFFF if op == 10 else simm)] += 1
        elif op in (24, 25):  # ori / oris
            if (w & 0xFFFF) >= 8:
                t["I:%d" % ((w & 0xFFFF) << (16 if op == 25 else 0))] += 1
        elif 32 <= op <= 55:  # lwz..stfdu: D(rA)
            ra = (w >> 16) & 31
            if ra not in (1, 2, 13) and simm >= 0x40:
                t["O:%d" % simm] += 1
    return t


def calls_and_branches(words):
    calls = sum(1 for w in words if (w >> 26) == 18 and (w & 1))
    branches = sum(1 for w in words if (w >> 26) == 16 or ((w >> 26) == 18 and not (w & 1)))
    return calls, branches


class Func:
    __slots__ = ("name", "file", "addr", "size", "tokens", "calls", "branches", "callees")

    def summary(self):
        return "%s %s size=%d calls=%d tokens=%d" % (self.file, self.name, self.size, self.calls, sum(self.tokens.values()))


# ---------------------------------------------------------------- GameCube objects (ELF32 BE)

R_PPC_ADDR32, R_PPC_ADDR16_LO, R_PPC_REL24, R_PPC_EMB_SDA21 = 1, 4, 10, 109


def _elf(path):
    d = open(path, "rb").read()
    shoff, = struct.unpack_from(">I", d, 0x20)
    shentsize, shnum, shstrndx = struct.unpack_from(">HHH", d, 0x2E)
    secs = []
    for i in range(shnum):
        name, typ, flags, addr, off, size, link, info, align, entsize = struct.unpack_from(">10I", d, shoff + i * shentsize)
        secs.append(dict(name=name, type=typ, off=off, size=size, link=link, info=info, data=d[off:off + size] if typ != 8 else b""))
    shstr = secs[shstrndx]["data"]
    for s in secs:
        e = shstr.find(b"\0", s["name"])
        s["name"] = shstr[s["name"]:e].decode()
    return secs


def gc_object_funcs(path, file):
    secs = _elf(path)
    symtab = next((s for s in secs if s["type"] == 2), None)
    if not symtab:
        return []
    strtab = secs[symtab["link"]]["data"]
    syms = []
    for j in range(len(symtab["data"]) // 16):
        nm, val, size, info, other, shndx = struct.unpack_from(">IIIBBH", symtab["data"], j * 16)
        e = strtab.find(b"\0", nm)
        syms.append((strtab[nm:e].decode(errors="replace"), val, size, info & 0xF, shndx))
    # relocations per section index
    relocs = {}
    for s in secs:
        if s["type"] == 4:  # RELA
            lst = relocs.setdefault(s["info"], [])
            for k in range(len(s["data"]) // 12):
                off, info, add = struct.unpack_from(">IIi", s["data"], k * 12)
                lst.append((off, info & 0xFF, info >> 8, add))

    def read_target(symidx, add, width):
        nm, val, size, typ, shndx = syms[symidx]
        if not (0 < shndx < len(secs)):
            return None, None
        sec = secs[shndx]
        o = val + add
        return sec, o

    out = []
    for idx, s in enumerate(secs):
        if not s["name"].startswith(".text") or s["type"] != 1:
            continue
        rl = sorted(relocs.get(idx, []))
        for nm, val, size, typ, shndx in syms:
            if shndx != idx or typ != 2 or size == 0:
                continue
            words = struct.unpack_from(">%dI" % (size // 4), s["data"], val)
            f = Func()
            f.name, f.file, f.addr, f.size = nm, file, val, size
            f.calls, f.branches = calls_and_branches(words)
            f.callees = []
            tok = Counter()
            rsites = set()
            for off, rtyp, symidx, add in rl:
                if not (val <= off < val + size):
                    continue
                wi = (off - val) // 4
                rsites.add(wi)
                if rtyp == R_PPC_REL24:
                    f.callees.append(syms[symidx][0])
                    continue
                if rtyp not in (R_PPC_ADDR16_LO, R_PPC_EMB_SDA21):
                    continue
                op = words[wi] >> 26
                sec, o = read_target(symidx, add, 4)
                if sec is None or not sec["data"]:
                    continue
                if op in (48, 50) and o + 8 <= len(sec["data"]):
                    v = struct.unpack_from(">f" if op == 48 else ">d", sec["data"], o)[0]
                    tok[float_key(abs(v))] += 1
                else:
                    e = sec["data"].find(b"\0", o)
                    raw = sec["data"][o:e]
                    if len(raw) >= 3 and all(0x20 <= b < 0x7F for b in raw):
                        tok["S:" + raw.decode()] += 1
            tok.update(insn_tokens(words, rsites))
            f.tokens = tok
            out.append(f)
    return out


def gc_functions(tww):
    """all GameCube functions with features; file = source file basename"""
    root = os.path.join(tww, "build", "GZLE01")
    out = []
    for d, _, files in os.walk(root):
        if "/obj" not in d:
            continue
        for fn in files:
            if fn.endswith(".o"):
                out.extend(gc_object_funcs(os.path.join(d, fn), fn[:-2] + ".cpp" if not fn[:-2].endswith((".c", ".cpp")) else fn[:-2]))
    return out


# ---------------------------------------------------------------- WWHD


def wwhd_functions(x):
    p = x.p
    out = {}
    for i, a in enumerate(x.funcs):
        end = x.funcs[i + 1] if i + 1 < len(x.funcs) else p.text_hi
        words = [p.word(k) for k in range(a, end, 4)]
        f = Func()
        f.name, f.file, f.addr, f.size = None, None, a, end - a
        f.calls, f.branches = calls_and_branches(words)
        rsites = {(site - a) // 4 for site, _ in x.refs.get(a, [])} | {
            (site - a) // 4 - 1 for site, _ in x.refs.get(a, [])}  # the lis of a lis/addi pair too
        tok = Counter(float_key(abs(v)) for v in x.floats(a))
        tok.update("S:" + s for _, _, s in x.strings(a) if len(s) >= 3 and all(" " <= ch <= "~" for ch in s))
        tok.update(insn_tokens(words, rsites))
        f.tokens = tok
        f.callees = []
        out[a] = f
    return out


def similarity(g, w, idf):
    """weighted overlap of token multisets, damped by size/call mismatch (0..1)"""
    inter = sum(min(c, w.tokens[k]) * idf.get(k, 1.0) for k, c in g.tokens.items() if k in w.tokens)
    tot = sum(c * idf.get(k, 1.0) for k, c in (g.tokens | w.tokens).items())
    sim = inter / tot if tot else 0.0
    r = (min(g.size, w.size) + 64) / (max(g.size, w.size) + 64)
    c = (min(g.calls, w.calls) + 2) / (max(g.calls, w.calls) + 2)
    return sim * (0.5 + 0.5 * r) * (0.5 + 0.5 * c)
