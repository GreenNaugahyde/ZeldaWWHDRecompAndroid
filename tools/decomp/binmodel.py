"""Whole-program models of both binaries for matching.

GameCube (GC): the split objects of the decompilation's build (tww/build/GZLE01/**/obj/*.o) carry
real symbol names and relocations, so every function's callees, data references and every class's
virtual table are exact. Functions get a stable integer id (index into GC.funcs).

WWHD: cking.rpx functions (from tools/recomp discovery), callees from `bl`, data references from
ADDR16_LO relocations, virtual tables from runs of {delta:index, function} entries in .data/.rodata
(Green Hills/EDG layout: 8 bytes per slot, the function pointer in the second word).
"""
import os
import struct
from collections import Counter, defaultdict

from decomp_index import float_key
from features import R_PPC_ADDR16_LO, R_PPC_ADDR32, R_PPC_EMB_SDA21, R_PPC_REL24, _elf, calls_and_branches, insn_tokens
from gc_layout import demangle

R_PPC_ADDR16_HA = 6


def based_tokens(words, lo_at, rsites):
    """floats and strings read through a base register (MWCC in RELs: addi rB, pool@l; lfs f, d(rB))"""
    tok = Counter()
    base = {}
    for i, w in enumerate(words):
        op = w >> 26
        rd, ra = (w >> 21) & 31, (w >> 16) & 31
        simm = (w & 0xFFFF) - (0x10000 if w & 0x8000 else 0)
        if i in lo_at:
            base[rd] = lo_at[i]
            continue
        if op in (48, 50) and ra in base and i not in rsites:
            sec, o = base[ra]
            o += simm
            if 0 <= o and o + (4 if op == 48 else 8) <= len(sec["data"]):
                v = struct.unpack_from(">f" if op == 48 else ">d", sec["data"], o)[0]
                tok[float_key(abs(v))] += 1
            continue
        if op == 14 and ra in base and i not in rsites:
            sec, o = base[ra]
            o += simm
            if 0 <= o < len(sec["data"]):
                e = sec["data"].find(b"\0", o)
                raw = sec["data"][o:e]
                if len(raw) >= 3 and all(0x20 <= b < 0x7F for b in raw):
                    tok["S:" + raw.decode()] += 1
        if op in (7, 12, 13, 14, 15, 32, 33, 34, 35, 40, 41, 42, 43, 46):
            base.pop(rd, None)
        elif op in (20, 21, 23, 24, 25, 26, 27, 28, 29):
            base.pop(ra, None)
        elif op == 31:
            base.pop(rd, None)
            base.pop(ra, None)
        elif op == 18 and (w & 1):
            for r in (3, 4, 5, 6, 7, 8, 9, 10, 11, 12):
                base.pop(r, None)
    return tok


class GF:
    __slots__ = ("id", "name", "dem", "file", "module", "off", "size", "words", "callees", "drefs", "tokens",
                 "calls", "branches", "glob", "weak", "callers", "frefs", "path", "callsites")


class GC:
    def __init__(self, tww):
        self.funcs = []
        self.vtables = []      # (vt symbol, file, [slot function id or None])
        self.byname = defaultdict(list)   # mangled name -> ids
        root = os.path.join(tww, "build", "GZLE01")
        objs = []
        for d, _, files in os.walk(root):
            if "/obj" not in d:
                continue
            for fn in sorted(files):
                if fn.endswith(".o"):
                    module = os.path.relpath(d, root).split(os.sep)[0]
                    module = "main" if module == "obj" else module
                    objs.append((os.path.join(d, fn), module, fn[:-2] + ".cpp" if not fn[:-2].endswith((".c", ".cpp")) else fn[:-2]))
        self.root = root
        objs.sort()
        pending = []   # (func, [(symname, defined-local-id or None)]) to resolve after all objects
        self._vt_pending = []
        self._pt_pending = []   # data objects holding function pointers: (symbol, file, [(offset, target)])
        for path, module, file in objs:
            self._load(path, module, file, pending)
        # global symbol table: prefer main-module definitions
        glob = {}
        for f in self.funcs:
            if f.glob and (f.name not in glob or f.module == "main"):
                glob[f.name] = f.id
        self.glob = glob
        def res(refs):
            out = []
            for nm, local, *_ in refs:
                t = local if local is not None else glob.get(nm)
                if t is not None:
                    out.append(t)
            return out
        for f, refs, frefs in pending:
            f.callees = res(refs)
            f.callsites = [(r[2], local if local is not None else glob.get(nm)) for r in refs
                           for nm, local in [r[:2]]]
            f.frefs = res(frefs)
        self.ptables = [(nm, file, [(o, t if isinstance(t, int) else glob.get(t)) for o, t in slots])
                        for nm, file, slots in self._pt_pending]
        self.ptables = [(nm, file, [(o, t) for o, t in sl if t is not None]) for nm, file, sl in self.ptables]
        del self._pt_pending
        for f in self.funcs:
            f.callers = set()
        for f in self.funcs:
            for c in f.callees:
                self.funcs[c].callers.add(f.id)
        for vt, file, slots in self._vt_pending:
            self.vtables.append((vt, file, [s if isinstance(s, int) or s is None else glob.get(s) for s in slots]))
        del self._vt_pending

    def _load(self, path, module, file, pending):
        secs = _elf(path)
        symtab = next((s for s in secs if s["type"] == 2), None)
        if not symtab:
            return
        strtab = secs[symtab["link"]]["data"]
        syms = []
        for j in range(len(symtab["data"]) // 16):
            nm, val, size, info, other, shndx = struct.unpack_from(">IIIBBH", symtab["data"], j * 16)
            e = strtab.find(b"\0", nm)
            syms.append((strtab[nm:e].decode(errors="replace"), val, size, info & 0xF, info >> 4, shndx))
        relocs = defaultdict(list)
        for s in secs:
            if s["type"] == 4:
                for k in range(len(s["data"]) // 12):
                    off, info, add = struct.unpack_from(">IIi", s["data"], k * 12)
                    relocs[s["info"]].append((off, info & 0xFF, info >> 8, add))
        local_fn = {}   # (shndx, value) -> id
        fl = []
        for idx, s in enumerate(secs):
            if not s["name"].startswith(".text") or s["type"] != 1:
                continue
            for si, (nm, val, size, typ, bind, shndx) in enumerate(syms):
                if shndx != idx or typ != 2 or size == 0:
                    continue
                f = GF()
                f.id = len(self.funcs)
                f.name, f.file, f.module, f.off, f.size = nm, file, module, val, size
                f.dem = demangle(nm)
                f.path = path
                f.glob = bind in (1, 2)
                f.weak = bind == 2
                f.words = struct.unpack_from(">%dI" % (size // 4), s["data"], val)
                f.calls, f.branches = calls_and_branches(f.words)
                self.funcs.append(f)
                self.byname[nm].append(f.id)
                local_fn[(idx, val)] = f.id
                fl.append((idx, f))

        def target(symidx):
            nm, val, size, typ, bind, shndx = syms[symidx]
            if shndx and (shndx, val) in local_fn:
                return nm, local_fn[(shndx, val)]
            return nm, None

        for idx, f in fl:
            s = secs[idx]
            refs, drefs, frefs = [], [], []
            tok = Counter()
            rsites = set()
            lo_at = {}
            for off, rtyp, symidx, add in relocs.get(idx, ()):
                if not (f.off <= off < f.off + f.size):
                    continue
                wi = (off - f.off) // 4
                rsites.add(wi)
                if rtyp == R_PPC_REL24:
                    refs.append(target(symidx) + (wi,))
                    continue
                if rtyp not in (R_PPC_ADDR16_LO, R_PPC_EMB_SDA21):
                    continue
                nm, val, size, typ, bind, shndx = syms[symidx]
                if typ == 2 or (shndx and secs[shndx]["name"].startswith(".text")):
                    frefs.append(target(symidx))   # address of a function taken (callback)
                    continue
                if not shndx:
                    frefs.append(target(symidx))   # resolved later: functions only
                if nm and not nm.startswith("@") and not nm.startswith("..."):
                    drefs.append(nm if bind != 0 else file + ":" + nm)
                if not (0 < shndx < len(secs)) or not secs[shndx]["data"]:
                    continue
                sec = secs[shndx]
                o = val + add
                op = f.words[wi] >> 26
                if op == 14:
                    lo_at[wi] = (sec, o)
                if op in (48, 50) and o + 8 <= len(sec["data"]):
                    v = struct.unpack_from(">f" if op == 48 else ">d", sec["data"], o)[0]
                    tok[float_key(abs(v))] += 1
                else:
                    e = sec["data"].find(b"\0", o)
                    raw = sec["data"][o:e]
                    if len(raw) >= 3 and all(0x20 <= b < 0x7F for b in raw):
                        tok["S:" + raw.decode()] += 1
            if lo_at:
                tok.update(based_tokens(f.words, lo_at, rsites))
            tok.update(insn_tokens(f.words, rsites))
            f.tokens = tok
            f.drefs = drefs
            pending.append((f, refs, frefs))
        # data objects with function pointers (method tables, PTMF tables, vtables)
        for si, (nm, val, size, typ, bind, shndx) in enumerate(syms):
            if typ != 1 or not shndx or shndx >= len(secs) or secs[shndx]["name"].startswith(".text") or size < 8:
                continue
            sl = []
            for off, rtyp, symidx, add in relocs.get(shndx, ()):
                if val <= off < val + size and rtyp == R_PPC_ADDR32:
                    tn, tid = target(symidx)
                    t = syms[symidx]
                    if tid is not None or not t[5]:
                        sl.append((off - val, tid if tid is not None else tn))
            sl.sort()
            if len(sl) >= 1:
                self._pt_pending.append((nm if bind else file + ":" + nm, file, sl))
        # virtual tables
        for nm, val, size, typ, bind, shndx in syms:
            if not nm.startswith("__vt__") or not shndx or shndx >= len(secs) or size < 12:
                continue
            rl = {off: (rtyp, symidx, add) for off, rtyp, symidx, add in relocs.get(shndx, ())}
            slots = []
            for o in range(val + 8, val + size, 4):
                r = rl.get(o)
                if r and r[0] == R_PPC_ADDR32:
                    tn, tid = target(r[1])
                    if tid is None and tn.startswith("__RTTI__"):
                        slots.append("|")   # header of a secondary table
                        continue
                    slots.append(tid if tid is not None else tn)
                else:
                    slots.append(None)
            # split at secondary-table headers (rtti, offset)
            parts, cur = [], []
            i = 0
            while i < len(slots):
                if slots[i] == "|":
                    parts.append(cur)
                    cur = []
                    i += 2
                    continue
                cur.append(slots[i])
                i += 1
            parts.append(cur)
            for k, p in enumerate(parts):
                self._vt_pending.append((nm + ("" if k == 0 else "#%d" % k), file, p))


class WW:
    """WWHD side; wraps an Xref"""

    def __init__(self, x):
        self.x = x
        p = x.p
        self.funcs = x.funcs
        self.pos = {f: i for i, f in enumerate(x.funcs)}
        self.size = {}
        self.callees = {}
        self.words = {}
        for i, a in enumerate(x.funcs):
            end = x.funcs[i + 1] if i + 1 < len(x.funcs) else p.text_hi
            self.size[a] = end - a
            out = []
            for s in range(a, end, 4):
                w = p.word(s)
                if (w >> 26) == 18 and s not in p.import_calls and s not in p.undef_calls:
                    t = ((w & 0x03FFFFFC) - (0x04000000 if w & 0x02000000 else 0) + (0 if w & 2 else s)) & 0xFFFFFFFF
                    # calls, and tail calls (b to another function's entry)
                    if p.in_text(t) and ((w & 1) or (t in self.pos and t != a)):
                        out.append(t)
            self.callees[a] = out
        self.callers = defaultdict(set)
        for a, cs in self.callees.items():
            for c in cs:
                self.callers[c].add(a)
        # data references (ADDR16_LO targets outside .text)
        self.drefs = {a: [t for _, t in x.refs.get(a, ()) if not p.in_text(t)] for a in x.funcs}
        self._vtables()

    def body(self, a):
        p = self.x.p
        return [p.word(k) for k in range(a, a + self.size[a], 4)]

    def _vtables(self):
        r = self.x.p.rpx
        ptr = {}
        for sec, addr, typ, sym, add in r.relocs:
            if sec.name in (".data", ".rodata") and typ == R_PPC_ADDR32 and not sym.import_lib:
                ptr[addr] = (sym.value + add) & 0xFFFFFFFF
        self.ptr = ptr
        secs = [r.by_name[".data"], r.by_name[".rodata"]]

        def rd(a):
            for s in secs:
                if s.addr <= a <= s.addr + s.size - 4:
                    return struct.unpack_from(">I", s.data, a - s.addr)[0]
            return None
        fset = set(self.funcs)
        self.vtables = []   # (address of first slot, [function addresses])
        seen = set()
        for a in sorted(ptr):
            if a in seen or ptr[a] not in fset:
                continue
            # slot = (delta:index word, function pointer); the vtable starts after a zero header slot
            if rd(a - 4) is None or (rd(a - 4) & 0xFFFF) != 0 or (a - 4) in ptr:
                continue
            if not (rd(a - 8) == 0 and (a - 8) not in ptr):
                continue
            run = []
            b = a
            while b in ptr and ptr[b] in fset and (b - 4) not in ptr and (rd(b - 4) & 0xFFFF) == 0:
                run.append(ptr[b])
                seen.add(b)
                b += 8
            self.vtables.append((a, run))

    def ptr_runs(self):
        """runs of function pointers in data outside virtual tables: consecutive entries with one
        stride (<= 16 bytes); returns [(first address, [function addresses])]"""
        fset = set(self.funcs)
        invt = set()
        for a, run in self.vtables:
            for k in range(len(run)):
                invt.add(a + 8 * k)
        sites = sorted(s for s, t in self.ptr.items() if t in fset and s not in invt)
        runs, cur, stride = [], [], None
        for s in sites:
            if cur and s - cur[-1] <= 16 and (stride is None or s - cur[-1] == stride):
                stride = s - cur[-1]
                cur.append(s)
            else:
                if len(cur) >= 2:
                    runs.append(cur)
                cur, stride = [s], None
        if len(cur) >= 2:
            runs.append(cur)
        return [(r[0], [self.ptr[s] for s in r]) for r in runs]


def refine_functions(x):
    """Add function entries that tools/recomp's discovery merges into the preceding function:
    targets of tail calls (`b` from another function) and prologues right after an unconditional
    branch/return. Only for matching; the recompiler keeps its own list. Rebuilds x.refs."""
    import bisect as _b
    p = x.p
    F = x.funcs
    fs = set(F)
    lo, hi = p.text_lo, p.text_hi
    jt = set()
    for _bctr, (t, n) in p.jump_tables.items():
        jt.update(t + 4 * k for k in range(n))

    def term(w):
        return w in (0x4E800020, 0x4E800420) or ((w >> 26) == 18 and not (w & 1))

    def prol(w):
        return w == 0x7C0802A6 or (w >> 16) == 0x9421
    new = set()
    for a in range(lo + 4, hi, 4):
        if a not in fs and a not in jt and term(p.word(a - 4)) and prol(p.word(a)):
            new.add(a)
    for a in range(lo, hi, 4):
        w = p.word(a)
        if (w >> 26) == 18 and not (w & 3):
            t = (a + ((w & 0x03FFFFFC) - (0x04000000 if w & 0x02000000 else 0))) & 0xFFFFFFFF
            if lo <= t < hi and t not in fs and t not in jt and term(p.word(t - 4)):
                if _b.bisect_right(F, a) != _b.bisect_right(F, t):
                    new.add(t)
    x.funcs = sorted(fs | new)
    x.added_funcs = new
    refs = {}
    for v in x.refs.values():
        for site, tgt in v:
            refs.setdefault(x.func_of(site), []).append((site, tgt))
    for v in refs.values():
        v.sort()
    x.refs = refs
    return len(new)
