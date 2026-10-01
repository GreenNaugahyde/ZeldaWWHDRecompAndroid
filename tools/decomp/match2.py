"""Second-generation matcher: grows trusted WWHD <-> GameCube pairs to a fixed point.

All evidence is scored on GameCube function ids (binmodel.GC), so overloads, static functions with
the same name in different actors, and weak copies of inline functions are kept apart.

Stages (each pair records the stage that found it):
  vtable    virtual tables aligned slot by slot with already matched slots as anchors
  graph     call-graph neighbourhood + content (BinDiff-like), mutual best with margin
  tu        unmatched functions between two matched functions of the same source file
            (WWHD keeps translation units contiguous), scored like `graph`
  dup       WWHD keeps a copy of an inline function per translation unit; identical bodies of a
            matched function get the same name (bodies of >= 4 instructions only)
"""
import bisect
import os
import re
import math
from collections import Counter, defaultdict

from features import similarity, wwhd_functions
from shape import cosine, ldiff, shape, wjaccard
from layout import accesses, align

PURE_VIRTUAL_MIN_USES = 50   # a slot target used by this many vtables is the pure-virtual stub


def body_key(words, base=None):
    """instruction words with branch targets made relative (identical code -> identical key)"""
    out = []
    for wd in words:
        op = wd >> 26
        if op == 18:
            out.append(0x48000000 | (wd & 3))
        else:
            out.append(wd)
    return tuple(out)


class Matcher:
    def __init__(self, g, w, log=print):
        self.g, self.w, self.log = g, w, log
        G = g.funcs
        # canonical GC id: weak/global functions defined in several modules are one function
        first = {}
        self.canon = []
        for f in G:
            if f.glob:
                self.canon.append(first.setdefault(f.name, f.id))
            else:
                self.canon.append(f.id)
        self.copies = Counter(self.canon)
        C = self.canon
        self.gcallees = defaultdict(set)
        self.gcallers = defaultdict(set)
        for f in G:
            for c in list(f.callees) + list(f.frefs):
                self.gcallees[C[f.id]].add(C[c])
                self.gcallers[C[c]].add(C[f.id])
        # calls plus taken function addresses (callbacks) on both sides
        fset = set(w.funcs)
        self.wcallees = {a: set(cs) for a, cs in w.callees.items()}
        for a in w.funcs:
            for _, t in w.x.refs.get(a, ()):
                if t in fset and t != a:
                    self.wcallees[a].add(t)
        self.wcallers = defaultdict(set)
        for a, cs in self.wcallees.items():
            for t in cs:
                self.wcallers[t].add(a)
        # content tokens
        self.W = wwhd_functions(w.x)
        df = Counter()
        for f in G:
            df.update(set(f.tokens))
        for f in self.W.values():
            df.update(set(f.tokens))
        n = len(G) + len(self.W)
        self.idf = {k: math.log(n / (1 + c)) for k, c in df.items()}
        # identical-body groups on the WWHD side
        self.wbody = {}
        groups = defaultdict(list)
        for a in w.funcs:
            k = body_key(w.body(a))
            self.wbody[a] = k
            groups[k].append(a)
        self.wgroup = {a: groups[self.wbody[a]] for a in w.funcs}
        # exact leaf bodies shared by both compilers (getters/setters)
        self.gleaf = {}
        for f in G:
            if f.size <= 48 and not f.calls:
                self.gleaf[f.id] = body_key(f.words)
        # GC order inside a file
        self.gfile_ids = defaultdict(list)
        for f in G:
            if self.canon[f.id] == f.id:
                self.gfile_ids[f.file].append(f.id)
        # compiler runtime (MWCC ptmf calls, exceptions, ...) has no counterpart in a GHS build
        self.excluded = {f.id for f in G if "/PowerPC_EABI_Support/Runtime/" in f.path}
        self.file_dir = {}
        for f in G:
            d = os.path.dirname(f.path.split("/obj/", 1)[-1])
            # WWHD links d/ and d/actor/ as one alphabetical run (d_a_* sorts among d_*)
            self.file_dir.setdefault(f.file, "d" if d == "d/actor" else d)
        self._between_cache = {}
        self._wshape, self._gshape = {}, {}
        self._chancache = {}
        self._fcache = {}
        self._gclass, self._gacc, self._wacc = {}, {}, {}
        self._fdone = set()
        self.vt_known = {}
        self._fvotes = defaultdict(lambda: defaultdict(Counter))
        self._actor_base = False
        self.M = {}            # WWHD address -> canonical GC id
        self.Minv = defaultdict(set)
        self.ev = {}
        self.prob = {}         # model probability of graph/tu pairs
        self.blocked = set()
        self.forbidden = set()  # (WWHD address, GC id) pairs known to be wrong   # WWHD addresses never to name (pure-virtual stub, conflicts)

    # ------------------------------------------------------------ state

    def ok_pair(self, a, c):
        if a in self.M or a in self.blocked or c in self.excluded:
            return False
        if (a, c) in self.forbidden:
            return False
        have = self.Minv.get(c)
        if have:
            # only identical copies may share a GameCube function
            if not all(self.wbody[b] == self.wbody[a] for b in have):
                return False
        return True

    def add(self, a, c, ev):
        if not self.ok_pair(a, c):
            return False
        self.M[a] = c
        self.Minv[c].add(a)
        self.ev[a] = ev
        return True

    def load_manual(self, path):
        """manual_names.tsv: hand-verified names (seeds) and known-wrong pairs"""
        n = 0
        for line in open(path):
            if not line.strip() or line.startswith("#"):
                continue
            a, sym = line.rstrip("\n").split("\t")[:2]
            a = int(a, 16)
            neg = sym.startswith("!")
            ids = self.g.byname.get(sym.lstrip("!"), [])
            for c in ids:
                if neg:
                    self.forbidden.add((a, self.canon[c]))
            if ids and not neg and a not in self.M:
                self.add(a, self.canon[ids[0]], "manual")
                n += 1
        return n

    # ------------------------------------------------------------ evidence

    def tu_labels(self):
        """file label per matched WWHD function whose GC function exists once (not a weak copy)"""
        lab = []
        for a in sorted(self.M):
            c = self.M[a]
            if self.copies[c] == 1 and len(self.wgroup[a]) == 1:
                lab.append((a, self.g.funcs[c].file, self.g.funcs[c].off))
        self.lab_addr = [a for a, _, _ in lab]
        self.lab = lab

    def tu_of(self, a, k=1):
        """(file, lo GC offset, hi GC offset) when the nearest labelled functions on both sides agree"""
        i = bisect.bisect_left(self.lab_addr, a)
        if i == 0 or i >= len(self.lab):
            return None
        (_, f1, o1), (_, f2, o2) = self.lab[i - 1], self.lab[i]
        if f1 == f2:
            return frozenset((f1,)), o1, o2
        if not self.BOUNDARY:
            return None
        # between two files: either of them, or a file that sorts between them in the same
        # directory (WWHD links each directory's units in alphabetical order)
        return self._between(f1, f2), None, None

    def _between(self, f1, f2):
        key = (f1, f2)
        if key not in self._between_cache:
            fs = {f1, f2}
            d1, d2 = self.file_dir.get(f1), self.file_dir.get(f2)
            lo, hi = sorted((f1.lower(), f2.lower()))
            for f, d in self.file_dir.items():
                if (d == d1 or d == d2) and lo < f.lower() < hi:
                    fs.add(f)
            self._between_cache[key] = frozenset(fs)
        return self._between_cache[key]

    def learn_data(self):
        """WWHD data address -> GC data symbol, from the data references of matched pairs
        (.data/.bss only: .rodata constants are covered by the float/string tokens)"""
        self.data_lo = self.w.x.p.rpx.by_name[".data"].addr
        co = defaultdict(Counter)
        cnt_a = Counter()
        for a, c in self.M.items():
            if self.ev.get(a) == "dup":
                continue
            ss = set(self.g.funcs[c].drefs)
            aa = {t for t in self.w.drefs.get(a, ()) if t >= self.data_lo}
            if not ss or not aa or len(ss) * len(aa) > 400:
                continue
            for x in aa:
                cnt_a[x] += 1
                for y in ss:
                    co[x][y] += 1
        amap = {}
        for x, v in co.items():
            (y, n), = v.most_common(1)
            if n >= 2 and n >= 0.7 * cnt_a[x]:
                amap[x] = y
        self.amap = amap
        # rarity of GC data symbols
        df = Counter()
        for f in self.g.funcs:
            df.update(set(f.drefs))
        self.dweight = {y: 1.0 / math.log2(2 + n) for y, n in df.items()}
        self.gusers = defaultdict(set)
        for f in self.g.funcs:
            for y in set(f.drefs):
                self.gusers[y].add(self.canon[f.id])

    def wsyms(self, a):
        return {self.amap[x] for x in self.w.drefs.get(a, ()) if x in self.amap}

    FEATURES = ("bias", "float", "has_float", "string", "imm", "off", "callee", "caller", "callee_miss",
                "gcallee_miss", "size", "ncalls", "leaf_equal", "data", "data_miss", "same_file", "in_window", "tiny",
                "op_cos", "bigram", "cbr_diff", "loop_diff", "cmp_diff", "fop_diff", "field_map")
    # logistic-regression weights, fitted on trusted pairs vs. same-file/neighbour decoys (train.py)
    BOUNDARY = __import__("os").environ.get("MATCH2_BOUNDARY", "1") == "1"
    PMIN = float(__import__("os").environ.get("MATCH2_PMIN", "0.6"))
    WEIGHTS = (-3.019, 3.256, -0.796, 3.479, 2.951, 0.970, 2.604, 0.437, -1.787, -1.663, -1.966, -0.273, 0.000,
               1.157, -0.529, 1.036, 2.583, 0.000, 0.661, 1.270, -0.452, 0.010, -0.792, -0.307, 1.454)

    def _chans(self, key, tokens):
        """per-function token channels: prefix -> (set, idf-weighted size); cached"""
        v = self._chancache.get(key)
        if v is None:
            idf = self.idf
            v = {}
            for k in tokens:
                p = k[:2]
                if p in ("F:", "S:", "I:", "O:"):
                    v.setdefault(p, set()).add(k)
            v = {p: (st, sum(idf.get(k, 1.0) for k in st)) for p, st in v.items()}
            self._chancache[key] = v
        return v

    def _chan(self, ca, cb, prefix):
        A = ca.get(prefix)
        B = cb.get(prefix)
        if not A and not B:
            return 0.0, 0
        if not A or not B:
            return 0.0, 1
        idf = self.idf
        sa, wa = A
        sb, wb = B
        if len(sa) > len(sb):
            sa, sb = sb, sa
        inter = sum(idf.get(k, 1.0) for k in sa if k in sb)
        uni = wa + wb - inter
        return (inter / uni if uni else 0.0), 1

    def feats(self, a, c, tu=None):
        g = self.g.funcs[c]
        W = self.W[a]
        M = self.M
        cal = car = 0.0
        cx = 0
        gce = self.gcallees.get(c, ())
        for t in self.wcallees.get(a, ()):
            m = M.get(t)
            if m is None:
                continue
            if m in gce:
                cal += 1.0 / math.log2(2 + len(self.wcallers.get(t, ())))
            else:
                cx += 1
        gcr = self.gcallers.get(c, ())
        for t in self.wcallers.get(a, ()):
            m = M.get(t)
            if m is not None and m in gcr:
                car += 1.0 / math.log2(2 + len(self.wcallees.get(t, ())))
        wmapped = {M[t] for t in self.wcallees.get(a, ()) if t in M}
        gx = sum(1 for t in gce if t in self.Minv and t not in wmapped)
        ca, cb = self._chans(("w", a), W.tokens), self._chans(("g", c), g.tokens)
        fj, fh = self._chan(ca, cb, "F:")
        sj, _ = self._chan(ca, cb, "S:")
        ij, _ = self._chan(ca, cb, "I:")
        oj, _ = self._chan(ca, cb, "O:")
        size = abs(math.log((W.size + 16) / (g.size + 16)))
        ncalls = abs(math.log((W.calls + 1) / (g.calls + 1)))
        leq = 1.0 if (c in self.gleaf and self.gleaf[c] == self.wbody[a]) else 0.0
        ws = self.wsyms(a) if hasattr(self, "amap") else set()
        dd = dx = 0.0
        if ws:
            gs = set(g.drefs)
            dd = sum(self.dweight.get(y, 0) for y in ws & gs)
            dx = sum(self.dweight.get(y, 0) for y in ws - gs)
        sf = iw = 0.0
        if tu is not None and g.file in tu[0]:
            sf = 1.0
            if tu[1] is not None and tu[1] < g.off < tu[2]:
                iw = 1.0
        tiny = 1.0 if W.size <= 8 and g.size <= 8 else 0.0
        ws, gsh = self.wshape(a), self.gshape(c)
        return (1.0, fj, fh, sj, ij, oj, cal, car, math.log1p(cx), math.log1p(gx), size, ncalls, leq, dd, dx,
                sf, iw, tiny, cosine(ws.hist, gsh.hist, ws.hnorm, gsh.hnorm),
                wjaccard(ws.bigrams, gsh.bigrams, ws.bsum, gsh.bsum),
                ldiff(ws.cbr, gsh.cbr), ldiff(ws.loops, gsh.loops), ldiff(ws.cmps, gsh.cmps), ldiff(ws.fops, gsh.fops),
                self.field_sim(a, c))

    # ---------------------------------------------------------- field offsets (per-class maps)

    def gclass(self, c):
        """class of the object a GC function works on (`this`, or a first pointer argument)"""
        cl = self._gclass.get(c)
        if cl is None:
            nm = self.g.funcs[c].name
            m_ = re.search(r"__(\d+)([A-Za-z_]\w*?)F", nm) or re.search(r"__F[PR](\d+)([A-Za-z_]\w*)", nm)
            cl = m_.group(2)[:int(m_.group(1))] if m_ else ""
            self._gclass[c] = cl
        return cl

    def gacc(self, c):
        v = self._gacc.get(c)
        if v is None:
            v = self._gacc[c] = accesses(self.g.funcs[c].words)
        return v

    def wacc(self, a):
        v = self._wacc.get(a)
        if v is None:
            v = self._wacc[a] = accesses(self.w.body(a))
        return v

    def learn_fields(self):
        """GC field offset -> WWHD offset per class, by aligning the this-accesses of matched pairs"""
        for a, c in list(self.M.items()):
            if a in self._fdone or self.ev.get(a) in ("dup", "dup-graph"):
                continue
            self._fdone.add(a)
            cl = self.gclass(c)
            if not cl:
                continue
            A, B = self.gacc(c), self.wacc(a)
            if not A or not B or len(A) * len(B) > 20000:
                continue
            for go, wo, k in align(A, B):
                self._fvotes[cl][(go, k)][wo] += 1
        self._fcache = {k: v for k, v in self._fcache.items() if k[0] == "w"}
        maps = {}
        for cl, v in self._fvotes.items():
            mp = {}
            for (go, k), cnt in v.items():
                wo, n = cnt.most_common(1)[0]
                if n >= 2 and n >= 0.6 * sum(cnt.values()):
                    mp[go] = wo
            if mp:
                keys = sorted(mp)
                maps[cl] = (mp, keys)
        self.fmaps = maps

    def translate(self, cl, go):
        mp = self.fmaps.get(cl)
        if not mp and self._actor_base:
            mp = self.fmaps.get("fopAc_ac_c")
        if not mp:
            return go
        exact, keys = mp
        if go in exact:
            return exact[go]
        i = bisect.bisect_right(keys, go) - 1
        if i < 0:
            return go
        return go + exact[keys[i]] - keys[i]

    def field_sim(self, a, c):
        if not hasattr(self, "fmaps"):
            return 0.0
        A, B = self.gacc(c), self.wacc(a)
        if not A or not B:
            return 0.0
        ta = self._fcache.get(("g", c))
        if ta is None:
            cl = self.gclass(c)
            self._actor_base = self.g.funcs[c].file.startswith("d_a_")
            ta = Counter((k, self.translate(cl, go)) for k, go in A if go >= 0)
            ta = self._fcache[("g", c)] = (ta, sum(ta.values()))
        tb = self._fcache.get(("w", a))
        if tb is None:
            tb = Counter((k, wo) for k, wo in B if wo >= 0)
            tb = self._fcache[("w", a)] = (tb, sum(tb.values()))
        return wjaccard(ta[0], tb[0], ta[1], tb[1])

    def wshape(self, a):
        s = self._wshape.get(a)
        if s is None:
            s = self._wshape[a] = shape(self.w.body(a))
        return s

    def gshape(self, c):
        s = self._gshape.get(c)
        if s is None:
            s = self._gshape[c] = shape(self.g.funcs[c].words)
        return s

    def score(self, a, c, tu=None):
        f = self.feats(a, c, tu)
        return sum(x * y for x, y in zip(f, self.WEIGHTS))

    # ------------------------------------------------------------ stages

    def _tables(self):
        g, w = self.g, self.w
        uses = Counter(t for _, s in w.vtables for t in s)
        self.pure = {t for t, k in uses.items() if k >= PURE_VIRTUAL_MIN_USES and w.size[t] <= 8}
        self.blocked |= self.pure
        C = self.canon
        gt, seen = [], set()
        for nm, file, slots in g.vtables:
            key = tuple(C[t] if t is not None else None for t in slots)
            if len(key) >= 2 and ("vt", key) not in seen:
                seen.add(("vt", key))
                gt.append(("vt", nm, key))
        for nm, file, sl in g.ptables:
            if nm.startswith("__vt__"):
                continue
            key = tuple(C[t] for _, t in sl)
            if len(key) >= 2 and ("pt", key) not in seen:
                seen.add(("pt", key))
                gt.append(("pt", nm, key))
        wt = [("vt", a, tuple(None if t in self.pure else t for t in s)) for a, s in w.vtables if len(s) >= 1]
        wt += [("pt", a, tuple(s)) for a, s in w.ptr_runs()]
        ginv = defaultdict(list)
        for j, (_, _, key) in enumerate(gt):
            for p, c in enumerate(key):
                if c is not None:
                    ginv[c].append((j, p))
        self._gt, self._wt, self._ginv = gt, wt, ginv

    def learn_vtrefs(self):
        """WWHD vtable (first-slot address) -> GameCube __vt__ symbol, from matched pairs whose
        code stores the table (constructors, destructors): WWHD code addresses a table 12 bytes
        before its first slot"""
        starts = {a for a, s in self.w.vtables}
        co = defaultdict(Counter)
        for a, c in self.M.items():
            if self.ev.get(a) in ("dup", "dup-graph", "dup-file"):
                continue
            gv = {d for d in self.g.funcs[c].drefs if d.startswith("__vt__")}
            wv = {t + 12 for _, t in self.w.x.refs.get(a, ()) if t + 12 in starts}
            if not gv or not wv or len(gv) * len(wv) > 36:
                continue
            if len(gv) == 1 and len(wv) == 1:
                co[next(iter(wv))][next(iter(gv))] += 2
            else:
                for t in wv:
                    for d in gv:
                        co[t][d] += 1.0 / max(len(gv), len(wv))
        self.vtname = {}
        for t, v in co.items():
            (d, n), = v.most_common(1)
            if n >= 2 and n >= 0.7 * sum(v.values()):
                self.vtname[t] = d

    def stage_vtables(self):
        """virtual tables and other function-pointer tables (method tables, PTMF tables)"""
        if not hasattr(self, "_gt"):
            self._tables()
            self._gt_byname = defaultdict(list)
            seen = set()
            for nm, file, slots in self.g.vtables:     # including one-slot tables
                key = tuple(self.canon[t] if t is not None else None for t in slots)
                if key and (nm, key) not in seen:
                    seen.add((nm, key))
                    self._gt.append(("vt1", nm, key))
                    self._gt_byname[nm].append(len(self._gt) - 1)
        self.learn_vtrefs()
        gt, wt, ginv = self._gt, self._wt, self._ginv
        M = self.M
        votes = defaultdict(Counter)
        self.vt_classes = {}
        for kind, wa, ws in wt:
            sv = Counter()
            for i, t in enumerate(ws):
                if t is None or t not in M:
                    continue
                lst = ginv.get(M[t], ())
                if len(lst) > 200:
                    continue
                for j, p in lst:
                    if gt[j][0] == kind or kind == "pt":
                        sv[(j, i - p)] += 1
            scored = []
            for (j, d), n in sv.items():
                if n < 2:
                    continue
                key = gt[j][2]
                dis = 0
                for i, t in enumerate(ws):
                    k = i - d
                    if t is not None and t in M and 0 <= k < len(key) and key[k] is not None and key[k] != M[t]:
                        dis += 1
                if dis:
                    continue
                exact = (len(ws) - d == len(key))
                scored.append((n + (0.5 if exact else 0), j, d))
            if not scored and kind == "vt" and wa in self.vtname:
                # no two matched slots, but the constructors/destructors that store this table are
                # matched to ones storing GameCube table N: align by length (WWHD actor classes
                # have one extra leading slot, the virtual destructor)
                for j in self._gt_byname.get(self.vtname[wa], ()):
                    key = gt[j][2]
                    for d in (0, 1) if len(key) > 1 else (0,):
                        if len(ws) - d == len(key):
                            scored.append((1.0, j, d))
                            break
                if len({gt[j][2] for _, j, _ in scored}) > 1:
                    scored = []
            if not scored:
                continue
            scored.sort(reverse=True)
            top = [x for x in scored if x[0] == scored[0][0]]
            if kind == "vt":
                self.vt_classes[wa] = sorted({gt[j][1] for _, j, _ in top})
                if len(self.vt_classes[wa]) == 1:
                    self.vt_known[wa] = self.vt_classes[wa][0]
            for i, t in enumerate(ws):
                if t is None or t in M:
                    continue
                opts = set()
                for _, j, d in top:
                    k = i - d
                    key = gt[j][2]
                    opts.add(key[k] if 0 <= k < len(key) else "-")
                if len(opts) == 1:
                    c = opts.pop()
                    if c not in ("-", None):
                        votes[t][c] += 1
        self._vt_copies(votes)
        added = 0
        for t, v in votes.items():
            if t in M or len(v) != 1:
                continue
            c = next(iter(v))
            if self.add(t, c, "vtable"):
                added += 1
        return added

    def _vt_copies(self, votes):
        """per-file copies of a class's vtable (inline virtuals are emitted per file): a table with the
        same slot bodies as an identified one, in a file whose GameCube object has a copy of exactly
        one of the classes known for that shape"""
        if not hasattr(self, "_gvt_files"):
            self._gvt_files = defaultdict(set)
            for nm, file, slots in self.g.vtables:
                self._gvt_files[nm].add(file)
        self.tu_labels()

        def sig(ws):
            return tuple(self.wbody[t] if t is not None else None for t in ws)
        known = defaultdict(set)
        for kind, wa, ws in self._wt:
            if kind == "vt" and wa in self.vt_known:
                known[sig(ws)].add(self.vt_known[wa])
        gt = self._gt
        for kind, wa, ws in self._wt:
            if kind != "vt" or wa in self.vt_known:
                continue
            names = known.get(sig(ws))
            if not names:
                continue
            files = Counter()
            for t in ws:
                if t is not None:
                    tu = self.tu_of(t)
                    if tu and len(tu[0]) == 1:
                        files[next(iter(tu[0]))] += 1
            if not files:
                continue
            fl = files.most_common(1)[0][0]
            opts = [n for n in names if fl in self._gvt_files.get(n, ())]
            if len(opts) != 1:
                continue
            keys = {gt[j][2] for j in self._gt_byname.get(opts[0], ())}
            fits = [(key, d) for key in keys for d in ((0, 1) if len(key) > 1 else (0,)) if len(ws) - d == len(key)]
            if len({f[0] for f in fits}) != 1:
                continue
            key, d = fits[0]
            self.vt_known[wa] = opts[0]
            for i, t in enumerate(ws):
                k = i - d
                if t is not None and t not in self.M and 0 <= k < len(key) and key[k] is not None:
                    votes[t][key[k]] += 1

    MARGIN = float(__import__("os").environ.get("MATCH2_MARGIN", "2.0"))
    FLOOR = float(__import__("os").environ.get("MATCH2_FLOOR", "-1.5"))

    def stage_graph(self, use_tu=True, pmin=None, ev="graph", margin=None):
        if pmin is None:
            pmin = self.PMIN
        """candidate pairs from matched neighbours (and file gaps), mutual best with margin"""
        M = self.M
        self.tu_labels()
        self.learn_data()
        self.learn_fields()
        cand = defaultdict(set)
        # functions sharing a rare data symbol
        for a in self.w.funcs:
            if a in M or a in self.blocked:
                continue
            for y in self.wsyms(a):
                us = self.gusers.get(y, ())
                if len(us) <= 12:
                    cand[a].update(c for c in us if c not in self.Minv)
        for a, c in list(M.items()):
            for wn, gn in ((self.wcallees.get(a, ()), self.gcallees.get(c, ())),
                           (self.wcallers.get(a, ()), self.gcallers.get(c, ()))):
                wu = [t for t in wn if t not in M and t not in self.blocked]
                gu = [t for t in gn if t not in self.Minv]
                if not wu or not gu or len(wu) * len(gu) > 900:
                    continue
                for t in wu:
                    cand[t].update(gu)
        tus = {}
        if use_tu:
            unmatched_by_file = defaultdict(list)
            for f, ids in self.gfile_ids.items():
                unmatched_by_file[f] = [c for c in ids if c not in self.Minv]
            for a in self.w.funcs:
                if a in M or a in self.blocked:
                    continue
                tu = self.tu_of(a)
                if tu:
                    tus[a] = tu
                    lst = [c for f in tu[0] for c in unmatched_by_file.get(f, ())]
                    if len(lst) <= self.TU_CAP:
                        cand[a].update(lst)
        best_w = {}
        best_g = {}
        for a, cs in cand.items():
            tu = tus.get(a) or self.tu_of(a)
            wsz = self.w.size[a]
            cs = [c for c in cs if c not in self.excluded and (a, c) not in self.forbidden
                  and abs(math.log((wsz + 16) / (self.g.funcs[c].size + 16))) < 1.8]
            sc = []
            for c in cs:
                f = self.feats(a, c, tu)
                sc.append((sum(x * y for x, y in zip(f, self.WEIGHTS)), c, f))
            if not sc:
                continue
            sc.sort(key=lambda t: -t[0])
            mx = max(0.0, sc[0][0])
            z = math.exp(-mx) + sum(math.exp(t[0] - mx) for t in sc)
            p = math.exp(sc[0][0] - mx) / z
            best_w[a] = (p, sc[0][1], sc[0][2], sc[0][0], sc[1][0] if len(sc) > 1 else -1e9, tu)
            for s_, c, _f in sc[:3]:
                if s_ > best_g.get(c, (-1e9, None))[0]:
                    best_g[c] = (s_, a)
        added = 0
        for a, (p, c, f, s1, s2, tu) in best_w.items():
            if best_g[c][1] != a:
                continue
            if margin is not None:
                # relative decision inside one file's gap: clear winner among that file's unmatched
                # functions, even when the absolute score leaves room for "none of these"
                if p >= pmin or not tu or len(tu[0]) != 1 or self.g.funcs[c].file not in tu[0]:
                    continue
                if s1 < self.FLOOR or s1 - s2 < margin:
                    continue
            elif p < pmin:
                continue
            small = self.w.size[a] < 24 or self.g.funcs[c].size < 24
            if small and not (p >= 0.95 and (f[6] + f[7] >= 1.0 or f[12])):
                continue
            if self.add(a, c, ev):
                self.prob[a] = p
                added += 1
        return added

    def stage_dup(self):
        self.tu_labels()
        added = 0
        for a, c in list(self.M.items()):
            grp = self.wgroup[a]
            if len(grp) < 2 or self.w.size[a] < 16:
                continue
            gf = self.g.funcs[c]
            if not (gf.weak or self.copies[c] > 1):   # only inline (weak) functions have copies
                continue
            # identical bodies can belong to different inline functions (trivial destructors of
            # different classes): a copy is named only if the GameCube object of its own source file
            # has a copy of the same function
            for b in grp:
                if b not in self.M and self.copy_in_file(b, c) and self.add(b, c, "dup"):
                    added += 1
        return added

    def copy_in_file(self, b, c):
        if not hasattr(self, "_name_files"):
            self._name_files = defaultdict(set)
            for f in self.g.funcs:
                self._name_files[f.name].add(f.file)
        tu = self.tu_of(b)
        if not tu or len(tu[0]) != 1:
            return False
        (fl,) = tu[0]
        return fl in self._name_files[self.g.funcs[c].name]

    def stage_dupgroup(self):
        """identical-body WWHD copies (inline functions emitted per translation unit) as one node:
        the callers of all copies, once matched, must agree on one GameCube callee"""
        seen = set()
        added = 0
        for a in self.w.funcs:
            grp = self.wgroup[a]
            if len(grp) < 2 or grp[0] in seen or self.w.size[a] < 16:
                continue
            seen.add(grp[0])
            if any(b in self.M for b in grp):
                continue
            callers = set()
            for b in grp:
                callers |= self.wcallers.get(b, set())
            matched = [t for t in callers if t in self.M]
            if len(matched) < 3:
                continue
            votes = Counter()
            for t in matched:
                for c in self.gcallees.get(self.M[t], ()):
                    votes[c] += 1
            if not votes:
                continue
            (c, n), = votes.most_common(1)
            rivals = [k for k, v in votes.items() if v == n and k != c]
            if rivals or n < 3 or n < 0.75 * len(matched) or c in self.Minv or c in self.excluded:
                continue
            gf = self.g.funcs[c]
            if (min(gf.size, self.w.size[a]) + 32) / (max(gf.size, self.w.size[a]) + 32) < 0.4:
                continue
            for b in grp:
                if self.copy_in_file(b, c) and self.add(b, c, "dup-graph"):
                    added += 1
        return added

    def stage_dupfile(self):
        """inline-function copies named by source file: a WWHD copy inside file F's block is matched
        to the weak copy in F's GameCube object, decided by its matched callers"""
        if not hasattr(self, "_weak_by_file"):
            self._weak_by_file = defaultdict(list)
            for f in self.g.funcs:
                if f.weak and f.size >= 8:
                    self._weak_by_file[f.file].append(f.id)
        self.tu_labels()
        added = 0
        for a in self.w.funcs:
            if a in self.M or a in self.blocked or len(self.wgroup[a]) < 2 or self.w.size[a] < 12:
                continue
            tu = self.tu_of(a)
            if not tu or len(tu[0]) != 1:
                continue
            (fl,) = tu[0]
            wcal = [self.M[t] for t in self.wcallers.get(a, ()) if t in self.M]
            if not wcal:
                continue
            best = []
            for r in self._weak_by_file.get(fl, ()):
                gf = self.g.funcs[r]
                if (min(gf.size, self.w.size[a]) + 16) / (max(gf.size, self.w.size[a]) + 16) < 0.5:
                    continue
                gcal = {self.canon[x] for x in gf.callers}
                n = sum(1 for t in wcal if t in gcal)
                if n:
                    best.append((n, r))
            best.sort(reverse=True)
            if not best or (len(best) > 1 and best[1][0] == best[0][0]):
                continue
            c = self.canon[best[0][1]]
            if c in self.excluded or (a, c) in self.forbidden:
                continue
            if self.add(a, c, "dup-file"):
                added += 1
        return added

    def stage_dupnames(self):
        """inline copies by elimination: for each identical-body group, the GameCube functions its
        named members are known to be; inside one file's block, if exactly one copy of the group is
        unnamed and exactly one of those functions has a weak copy in that file's GameCube object
        (and is not named in the block yet), they are the same"""
        if not hasattr(self, "_name_files"):
            self.copy_in_file(self.w.funcs[0], 0)
        self.tu_labels()
        gnames = defaultdict(set)
        for a, c in self.M.items():
            gf = self.g.funcs[c]
            if len(self.wgroup[a]) >= 2 and (gf.weak or self.copies[c] > 1):   # inline functions only
                gnames[self.wbody[a]].add(c)
        blocks = defaultdict(lambda: defaultdict(list))   # file -> body -> unnamed copies
        named_in = defaultdict(set)                        # file -> canonical ids named in its block
        for a in self.w.funcs:
            tu = self.tu_of(a)
            if not tu or len(tu[0]) != 1:
                continue
            (fl,) = tu[0]
            if a in self.M:
                named_in[fl].add(self.M[a])
            elif len(self.wgroup[a]) >= 2 and self.w.size[a] >= 12 and a not in self.blocked:
                blocks[fl][self.wbody[a]].append(a)
        added = 0
        for fl, groups in blocks.items():
            for body, copies in groups.items():
                if len(copies) != 1:
                    continue
                opts = [c for c in gnames.get(body, ()) if c not in named_in[fl]
                        and fl in self._name_files[self.g.funcs[c].name]]
                if len(opts) == 1 and self.add(copies[0], opts[0], "dup-file"):
                    named_in[fl].add(opts[0])
                    added += 1
        return added

    TU_CAP = int(__import__("os").environ.get("MATCH2_TU_CAP", "400"))
    MARGIN_ON = __import__("os").environ.get("MATCH2_MARGIN_ON", "1") == "1"

    def run(self, rounds=40):
        phase2 = False
        for r in range(rounds):
            n0 = len(self.M)
            v = self.stage_vtables()
            gph = self.stage_graph(use_tu=False)
            t = self.stage_graph(use_tu=True, ev="tu")
            d = self.stage_dup() + self.stage_dupgroup() + self.stage_dupfile() + self.stage_dupnames()
            self.log("round %d: vtable +%d graph +%d tu +%d dup +%d -> %d" % (r + 1, v, gph, t, d, len(self.M)))
            if len(self.M) == n0:
                if phase2 or not self.MARGIN_ON:
                    break
                phase2 = True
            if phase2:
                mg = self.stage_graph(use_tu=True, ev="tu-margin", margin=self.MARGIN)
                self.log("        tu-margin +%d" % mg)
        return self.M
