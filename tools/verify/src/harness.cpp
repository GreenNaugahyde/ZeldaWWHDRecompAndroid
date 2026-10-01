/* Differential verification harness.
 *
 * For each WWHD function that has a candidate source implementation (VERIFY(addr, fn) in
 * wwhd_src), run the recompiled original and the candidate on identical inputs and compare:
 *   - the return value (type from the candidate's signature),
 *   - the net memory effect (every byte whose final value differs from its initial value,
 *     outside the stack below the entry SP),
 *   - the sequence of calls to other guest functions with their argument registers.
 * Callees are not executed (unless a unit links them as `real`): they are recorded and
 * answered by a mock (generated inputs) or with the results and memory effects recorded in
 * the game (recorded inputs, runtime/include/verify_tap.h).
 *
 * usage: verify [-n N] [-seed S] [-rec DIR] [-only ADDR] [-spec FILE] [-image FILE] [-v]
 */
#include <setjmp.h>

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>
#include <dirent.h>

#include "gabi.h"
#include "unit.h"
#include "verify_tap.h"
#include "vm.h"

namespace gabi {
thread_local Cpu* cpu;
static Candidate* g_cands;
Candidate::Candidate(u32 a, const char* n, EntryFn f, RetKind r) : addr(a), name(n), fn(f), ret(r), next(g_cands) { g_cands = this; }
}  // namespace gabi

extern "C" {
int g_ppc_trace;
volatile int g_core_preempt[3];
void ppc_trace_enter(uint32_t) {}
void ppc_preempt(Cpu*) {}
uint64_t ppc_timebase(void) { return 0; }
}

/* ------------------------------------------------------------------ hashing */
static inline uint64_t mix(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static inline uint64_t hash3(uint64_t a, uint64_t b, uint64_t c) { return mix(a ^ mix(b ^ mix(c))); }

static std::vector<uint32_t>* g_dict;
static float rand_float(uint64_t h) {
    if (g_dict && !g_dict->empty() && (h >> 50) % 4 == 0) {
        uint32_t v = (*g_dict)[(h >> 20) % g_dict->size()];
        switch ((h >> 40) % 4) { /* the constant or a neighbour */
        case 1: v += 1; break;
        case 2: v -= 1; break;
        default: break;
        }
        return u32_as_f32(v);
    }
    switch (h % 16) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return -1.0f;
    case 3: return 0.5f;
    case 4: return (float)((int)((h >> 8) % 64) - 32);  /* small integers */
    case 5: return (float)((h >> 8) % 100000) / 100.0f; /* 0..1000 */
    default: {
        double u = (double)((h >> 11) & 0xFFFFFFFull) / (double)0x10000000;
        double scale = (h >> 40) % 3 == 0 ? 10.0 : ((h >> 40) % 3 == 1 ? 1000.0 : 100000.0);
        return (float)((u * 2.0 - 1.0) * scale);
    }
    }
}

/* a plausible 32-bit word for memory nobody described: zero, small ints, small per-byte values,
 * floats, pointers into the random heap, s16 pairs, -1 patterns, random bits */
static uint32_t typed_word(uint64_t h) {
    uint32_t k = h % 100, x = (uint32_t)(h >> 20);
    if (k < 14) return 0;
    if (k < 26) return x % 8;
    if (k < 36) return ((x & 3) << 24) | (((x >> 2) & 3) << 16) | (((x >> 4) & 3) << 8) | ((x >> 6) & 3);
    if (k < 56) return f32_as_u32(rand_float(h >> 7)); /* includes dictionary constants */
    if (k < 72) return 0x30000000u + (x & 0x00FFFFF0u);
    if (k < 82) return ((x & 0xFFFF) << 16) | ((x >> 13) & 0xFFFF);
    if (k < 88) return (k & 1) ? 0xFFFFFFFFu : 0x0000FFFFu;
    return (uint32_t)(h >> 32) ^ x;
}

/* ------------------------------------------------------------------ memory model */
struct Page {
    uint8_t d[4096];
    uint8_t init[4096];
    uint16_t ep[4096]; /* clobber epoch the byte's value is current for */
    uint64_t have[64];
    uint64_t wr[64];
};
struct SnapPage {
    uint8_t d[4096];
    uint64_t has[64];
};
struct Region {
    uint32_t addr, size;
    std::vector<uint8_t> data;
};

enum Mode { MODE_GEN, MODE_REC };

struct CallEv {
    uint32_t target;
    int kind;
    uint32_t r[11];
    double f[9];
    int nint, nflt;                 /* candidate: declared arguments */
    std::vector<uint32_t> stk;      /* stack argument words (sp+8...) */
    std::vector<uint8_t> stkref[4]; /* pointee bytes when a stack argument points into the stack */
    std::vector<uint8_t> sref[11];  /* pointee bytes of stack pointer arguments */
};

struct Patch { uint32_t ea; uint8_t v; };
struct RecCall {
    uint32_t target;
    TapRegs at, after;
    std::vector<Patch> patches;
};

static struct VM {
    std::unordered_map<uint32_t, Page*> pages;
    std::vector<Page*> touched;
    std::unordered_map<uint32_t, SnapPage*> snap;
    std::vector<Region> image;
    uint64_t seed = 1;
    Mode mode = MODE_GEN;
    uint32_t entry_sp = 0, stack_lo = 0;
    /* run state */
    std::vector<CallEv> calls;
    std::unordered_map<uint32_t, uint32_t> per_target;
    int depth = 0;
    bool is_candidate = false;
    uint64_t budget = 0;
    jmp_buf abort_jmp;
    bool can_abort = false;
    std::string abort_reason;
    uint32_t stray = 0, first_stray = 0;
    /* generated mode: each mocked call starts a new epoch in which it may have changed any
     * heap/global word (not the image, not the scratch stack): 1 in kClobber per word */
    uint32_t epoch = 0;
    bool clobber = true;      /* -noclobber turns it off */
    bool clobber_now = true;  /* per input: on for half of the generated inputs (the other half
                               * keeps values across calls, so deep paths stay reachable) */
    /* constants the original reads from .rodata (collected by a pre-run): generated floats
     * come from this dictionary part of the time, so comparisons hit their boundaries */
    std::vector<uint32_t> dict;
    bool collect = false;
    std::vector<uint32_t>* cov = nullptr;
    uint32_t cov_func = 0;
    int traps = 0;
    /* recorded input */
    std::vector<RecCall> rec_calls;
    std::vector<CallEv>* orig_calls = nullptr; /* for translating stack patches to the candidate */
} V;

static const Region* image_region(uint32_t ea) {
    for (const Region& r : V.image)
        if (ea - r.addr < r.size) return &r;
    return nullptr;
}

/* initial value of a byte; *known: from snapshot / image / stack (not random) */
static uint8_t initial_byte(uint32_t ea, bool* known) {
    auto it = V.snap.find(ea >> 12);
    if (it != V.snap.end()) {
        uint32_t o = ea & 0xFFF;
        if (it->second->has[o >> 6] >> (o & 63) & 1) { *known = true; return it->second->d[o]; }
    }
    if (const Region* r = image_region(ea)) {
        *known = true;
        uint32_t o = ea - r->addr;
        return o < r->data.size() ? r->data[o] : 0;
    }
    if (ea - V.stack_lo < V.entry_sp - V.stack_lo) { *known = true; return 0; }
    *known = false;
    uint32_t w = typed_word(hash3(V.seed, ea & ~3u, 0x5EED));
    return (uint8_t)(w >> (24 - 8 * (ea & 3)));
}

static Page* page(uint32_t pn) {
    auto it = V.pages.find(pn);
    if (it != V.pages.end()) return it->second;
    Page* p = (Page*)calloc(1, sizeof(Page));
    V.pages[pn] = p;
    V.touched.push_back(p);
    return p;
}

static const uint32_t kClobber = 4;

static bool clobberable(uint32_t ea) {
    if (image_region(ea)) return false;
    if (ea - V.stack_lo < V.entry_sp + 8 - V.stack_lo) return false;
    return true;
}

static inline void materialize(Page* p, uint32_t ea, bool reading) {
    uint32_t o = ea & 0xFFF;
    if (!(p->have[o >> 6] >> (o & 63) & 1)) {
        bool known;
        uint8_t v = initial_byte(ea, &known);
        if (!known && reading && V.mode == MODE_REC && V.depth == 0) {
            if (!V.stray) V.first_stray = ea;
            V.stray++;
        }
        p->d[o] = p->init[o] = v;
        p->ep[o] = 0;
        p->have[o >> 6] |= 1ull << (o & 63);
    }
    if (p->ep[o] != V.epoch) {
        /* did a mocked call since the byte was last current overwrite its word? (latest wins) */
        if (V.clobber && V.clobber_now && V.mode == MODE_GEN && clobberable(ea))
            for (uint32_t e = V.epoch; e > p->ep[o]; e--) {
                uint64_t h = hash3(V.seed, ea & ~3u, 0xC10B + e);
                if (h % kClobber == 0) {
                    uint32_t w = typed_word(h >> 2);
                    p->d[o] = (uint8_t)(w >> (24 - 8 * (ea & 3)));
                    break;
                }
            }
        p->ep[o] = (uint16_t)V.epoch;
    }
}

static inline void tick() {
    if (++V.budget > 20000000ull && V.can_abort) {
        V.abort_reason = "access budget exceeded";
        longjmp(V.abort_jmp, 1);
    }
}

static inline uint8_t rd8(uint32_t ea) {
    Page* p = page(ea >> 12);
    materialize(p, ea, true);
    return p->d[ea & 0xFFF];
}
static inline void wr8(uint32_t ea, uint8_t v) {
    Page* p = page(ea >> 12);
    materialize(p, ea, false);
    uint32_t o = ea & 0xFFF;
    p->d[o] = v;
    p->wr[o >> 6] |= 1ull << (o & 63);
}
/* a callee's write (mock / recorded effect): changes memory but is not the function's own write */
static void patch8(uint32_t ea, uint8_t v) {
    Page* p = page(ea >> 12);
    materialize(p, ea, false);
    p->d[ea & 0xFFF] = v;
}
static uint8_t peek8(uint32_t ea) { /* no stray accounting */
    int d = V.depth;
    V.depth = 1;
    uint8_t v = rd8(ea);
    V.depth = d;
    return v;
}

extern "C" {
uint8_t vm_ld8(uint32_t ea) { tick(); return rd8(ea); }
uint16_t vm_ld16(uint32_t ea) { tick(); return (uint16_t)(rd8(ea) << 8 | rd8(ea + 1)); }
static int g_trace; /* -trace SEED: print the memory accesses and calls of that input */
uint32_t vm_ld32(uint32_t ea) {
    tick();
    uint32_t v = (uint32_t)rd8(ea) << 24 | (uint32_t)rd8(ea + 1) << 16 | (uint32_t)rd8(ea + 2) << 8 | rd8(ea + 3);
    if (g_trace && V.depth == 0) printf("        ld32 %08X -> %08X (epoch %u)\n", ea, v, V.epoch);
    if (V.collect && V.depth == 0 && ea - 0x10000000u < 0x0018C0B0u && (v & 0x7F800000u) != 0x7F800000u && V.dict.size() < 64 &&
        std::find(V.dict.begin(), V.dict.end(), v) == V.dict.end())
        V.dict.push_back(v); /* a .rodata word (float constant) */
    return v;
}
uint64_t vm_ld64(uint32_t ea) { return (uint64_t)vm_ld32(ea) << 32 | vm_ld32(ea + 4); }
void vm_st8(uint32_t ea, uint8_t v) { tick(); wr8(ea, v); }
void vm_st16(uint32_t ea, uint16_t v) { tick(); wr8(ea, v >> 8); wr8(ea + 1, (uint8_t)v); }
void vm_st32(uint32_t ea, uint32_t v) {
    tick();
    if (g_trace && V.depth == 0) printf("        st32 %08X <- %08X (epoch %u)\n", ea, v, V.epoch);
    for (int i = 0; i < 4; i++) wr8(ea + i, (uint8_t)(v >> (24 - 8 * i)));
}
void vm_st64(uint32_t ea, uint64_t v) { vm_st32(ea, (uint32_t)(v >> 32)); vm_st32(ea + 4, (uint32_t)v); }

uint8_t gmem_ld8(uint32_t ea) { return vm_ld8(ea); }
uint16_t gmem_ld16(uint32_t ea) { return vm_ld16(ea); }
uint32_t gmem_ld32(uint32_t ea) { return vm_ld32(ea); }
uint64_t gmem_ld64(uint32_t ea) { return vm_ld64(ea); }
void gmem_st8(uint32_t ea, uint8_t v) { vm_st8(ea, v); }
void gmem_st16(uint32_t ea, uint16_t v) { vm_st16(ea, v); }
void gmem_st32(uint32_t ea, uint32_t v) { vm_st32(ea, v); }
void gmem_st64(uint32_t ea, uint64_t v) { vm_st64(ea, v); }

void vm_cov(uint32_t, uint32_t block) {
    if (V.cov && V.depth == 0 && block < V.cov->size()) (*V.cov)[block]++;
}
void vm_trap(Cpu*, uint32_t, uint32_t) { V.traps++; }
}

static void reset_memory() {
    for (Page* p : V.touched) {
        memset(p->have, 0, sizeof p->have);
        memset(p->wr, 0, sizeof p->wr);
    }
}

static void clear_all() {
    for (Page* p : V.touched) free(p);
    V.touched.clear();
    V.pages.clear();
    for (auto& kv : V.snap) free(kv.second);
    V.snap.clear();
}

static void snap_set(uint32_t ea, uint8_t v) {
    SnapPage*& s = V.snap[ea >> 12];
    if (!s) s = (SnapPage*)calloc(1, sizeof(SnapPage));
    uint32_t o = ea & 0xFFF;
    s->d[o] = v;
    s->has[o >> 6] |= 1ull << (o & 63);
}
static bool snap_has(uint32_t ea) {
    auto it = V.snap.find(ea >> 12);
    if (it == V.snap.end()) return false;
    uint32_t o = ea & 0xFFF;
    return it->second->has[o >> 6] >> (o & 63) & 1;
}

/* net effect of a run: bytes whose value changed, outside the scratch stack */
static std::map<uint32_t, uint8_t> net_writes() {
    std::map<uint32_t, uint8_t> out;
    for (auto& kv : V.pages) {
        Page* p = kv.second;
        for (int w = 0; w < 64; w++) {
            uint64_t m = p->wr[w];
            while (m) {
                int b = __builtin_ctzll(m);
                m &= m - 1;
                uint32_t o = w * 64 + b, ea = (kv.first << 12) | o;
                if (ea - V.stack_lo < V.entry_sp + 8 - V.stack_lo) continue; /* own frames + back chain/LR save */
                materialize(p, ea, false); /* apply calls made after the write (clobber) */
                /* recorded inputs: a written byte whose initial value was never read is unknown,
                 * so it counts as written whatever its value */
                bool unknown = V.mode == MODE_REC && !snap_has(ea) && !image_region(ea);
                if (p->d[o] != p->init[o] || unknown) out[ea] = p->d[o];
            }
        }
    }
    return out;
}

/* ------------------------------------------------------------------ calls */
static const CalleeInfo* callee_info(uint32_t t) {
    static std::unordered_map<uint32_t, const CalleeInfo*> idx;
    if (idx.empty())
        for (unsigned i = 0; i < unit_ncallees; i++) idx[unit_callees[i].addr] = &unit_callees[i];
    auto it = idx.find(t);
    return it == idx.end() ? nullptr : it->second;
}

static bool in_stack(uint32_t v) { return v - V.stack_lo < V.entry_sp - V.stack_lo; }

static uint32_t typed_ret(uint64_t h) {
    uint32_t k = h % 100;
    if (k < 28) return 0;
    if (k < 54) return 1;
    if (k < 62) return 0xFFFFFFFFu; /* -1 / 0xFF / 0xFFFF sentinels after shaping */
    if (k < 75) return (uint32_t)(h >> 33) % 16;
    if (k < 90) return 0x30000000u + ((uint32_t)(h >> 20) & 0x00FFFFF0u);
    return (uint32_t)(h >> 32);
}

static std::unordered_map<uint32_t, std::pair<int64_t, int64_t>> g_ret_specs; /* unit `ret` lines: callee -> result range */

/* a result of the shape the real callee returns (GHS callers do not re-extend small results) */
static uint32_t shape_ret(const CalleeInfo* ci, uint32_t v) {
    if (!ci) return v;
    switch (ci->retkind) {
    case 1: if (ci->retbits < 32) { if (ci->retbits == 1) return v & 1; v &= (1u << ci->retbits) - 1; } return v;
    case 2: if (ci->retbits < 32) { uint32_t s = 32 - ci->retbits; v = (uint32_t)((int32_t)(v << s) >> s); } return v;
    case 3: return ci->retconst;
    default: return v;
    }
}

static std::string callee_name(uint32_t t);
static void do_call(Cpu* c, uint32_t target, int kind, int nint, int nflt) {
    const CalleeInfo* ci = callee_info(target);
    if (V.depth > 0) { /* inside a real callee: answer silently */
        uint64_t h = hash3(V.seed, target, 0xABCDEF + V.per_target[target]++);
        if (!ci || ci->defr >> 3 & 1) c->r[3] = shape_ret(ci, typed_ret(h));
        if (!ci || ci->deff >> 1 & 1) c->f[1].ps0 = c->f[1].ps1 = rand_float(h >> 3);
        return;
    }
    size_t k = V.calls.size();
    if (g_trace) printf("        call %s\n", callee_name(target).c_str());
    CallEv ev;
    ev.target = target;
    ev.kind = kind;
    memcpy(ev.r, c->r, sizeof ev.r);
    for (int i = 0; i < 9; i++) ev.f[i] = c->f[i].ps0;
    ev.nint = nint;
    ev.nflt = nflt;
    for (int i = 3; i <= 10; i++)
        if (in_stack(c->r[i])) {
            uint32_t n = ci && ci->ptrsz[i] ? ci->ptrsz[i] : 4;
            if (n == 255) { ev.sref[i].push_back(0); continue; } /* storage for a constructor: not an input */
            for (uint32_t j = 0; j < n; j++) ev.sref[i].push_back(peek8(c->r[i] + j));
        }
    {
        int ns = ci ? ci->nstack : 0;
        if (nint > 8) ns = std::max(ns, nint - 8);
        for (int j = 0; j < ns && j < 4; j++) {
            uint32_t w = 0;
            for (int b = 0; b < 4; b++) w = w << 8 | peek8(c->r[1] + 8 + 4 * j + b);
            ev.stk.push_back(w);
            if (in_stack(w))
                for (int b = 0; b < 4; b++) ev.stkref[j].push_back(peek8(w + b));
        }
    }
    V.calls.push_back(ev);

    if (ci && ci->real) {
        V.depth++;
        ci->real(c);
        V.depth--;
        return;
    }
    if (V.mode == MODE_REC) {
        if (k < V.rec_calls.size()) {
            const RecCall& rc = V.rec_calls[k];
            /* volatile state after the call, as recorded (r1/r2/r13-r31, f14-f31 are preserved) */
            Cpu after;
            tap_regs_to(&after, &rc.after);
            c->r[0] = after.r[0];
            for (int i = 3; i <= 12; i++) c->r[i] = after.r[i];
            for (int i = 0; i <= 13; i++) c->f[i] = after.f[i];
            for (int b : {0, 1, 2, 3, 4, 5, 6, 7, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31}) c->cr[b] = after.cr[b];
            c->ctr = after.ctr;
            c->xer_ca = after.xer_ca;
            c->xer_so = after.xer_so;
            c->xer_ov = after.xer_ov;
            /* memory effects of the callee seen by the function */
            V.depth++;
            for (const Patch& p : rc.patches) {
                uint32_t ea = p.ea;
                if (V.is_candidate && in_stack(ea)) {
                    /* a callee writing through a stack pointer argument: map to the candidate's local */
                    uint32_t best = 0, bi = 0;
                    for (int i = 3; i <= 10; i++) {
                        uint32_t a = rc.at.r[i];
                        if (a <= ea && ea - a < 256 && a >= best) { best = a; bi = i; }
                    }
                    if (bi) ea = ev.r[bi] + (p.ea - rc.at.r[bi]);
                }
                patch8(ea, p.v);
            }
            V.depth--;
            return;
        }
    }
    /* generated mock: answer in the registers the real callee may write; memory it may have
     * changed is modelled by the new epoch */
    V.epoch++;
    uint64_t h = hash3(V.seed, target, V.per_target[target]++);
    if (!ci || ci->defr >> 3 & 1) c->r[3] = shape_ret(ci, typed_ret(h));
    auto rs = g_ret_specs.find(target);
    if (rs != g_ret_specs.end() && (h >> 45) % 8 != 0) /* mostly in the declared domain */
        c->r[3] = (uint32_t)(rs->second.first + (int64_t)((h >> 13) % (uint64_t)(rs->second.second - rs->second.first + 1)));
    if (ci && ci->defr >> 4 & 1) c->r[4] = typed_ret(h >> 5);
    if (!ci || ci->deff >> 1 & 1) c->f[1].ps0 = c->f[1].ps1 = (double)rand_float(h >> 9);
    if (ci) {
        V.depth++;
        for (int i = 3; i <= 10; i++)
            if (ci->outp[i] && ci->ptrsz[i] && in_stack(c->r[i]))
                for (uint32_t j = 0; j < ci->ptrsz[i]; j += 4) {
                    uint32_t w = typed_word(hash3(h, i, j));
                    for (uint32_t b = 0; b < 4 && j + b < ci->ptrsz[i]; b++) patch8(c->r[i] + j + b, (uint8_t)(w >> (24 - 8 * b)));
                }
        V.depth--;
    }
}

extern "C" {
void vm_call(Cpu* c, uint32_t target, int kind) { do_call(c, target, kind, -1, -1); }
void gmem_call(Cpu* c, uint32_t target, int kind, int nint, int nflt) { do_call(c, target, kind, nint, nflt); }
}

/* ------------------------------------------------------------------ inputs */
struct FieldSpec {
    uint32_t func; /* 0 = all */
    int base_reg;  /* -1 absolute */
    uint32_t off;
    int size;
    bool is_float;
    double lo, hi;
};
static std::vector<FieldSpec> g_specs;

static void load_specs(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        char* h = strchr(line, '#');
        if (h) *h = 0;
        char kw[32], fn[32], where[64], ty[16];
        double lo, hi;
        int n = sscanf(line, "%31s %31s %63s %15s %lf %lf", kw, fn, where, ty, &lo, &hi);
        if (!strcmp(kw, "ret")) { /* ret ADDR LO HI: a callee's results (generated mocks) */
            long long a, b;
            if (sscanf(line, "%*s %31s %lld %lld", fn, &a, &b) == 3) g_ret_specs[(uint32_t)strtoul(fn, nullptr, 16)] = {a, b};
            continue;
        }
        if (n < 5 || strcmp(kw, "field")) continue;
        if (n == 5) hi = lo;
        FieldSpec s{};
        s.func = strcmp(fn, "*") ? (uint32_t)strtoul(fn, nullptr, 16) : 0;
        if (where[0] == 'r') {
            char* plus;
            s.base_reg = (int)strtol(where + 1, &plus, 10);
            s.off = *plus == '+' ? (uint32_t)strtoul(plus + 1, nullptr, 0) : 0;
        } else {
            s.base_reg = -1;
            s.off = (uint32_t)strtoul(where, nullptr, 16);
        }
        s.is_float = ty[0] == 'f';
        s.size = atoi(ty + 1) / 8;
        s.lo = lo;
        s.hi = hi;
        g_specs.push_back(s);
    }
    fclose(f);
}

static void load_image(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "no image %s (run mkimage.py)\n", path); exit(1); }
    uint32_t hdr[2];
    while (fread(hdr, 4, 2, f) == 2) {
        Region r;
        r.addr = hdr[0];
        r.size = hdr[1];
        r.data.resize(r.size);
        if (fread(r.data.data(), 1, r.size, f) != r.size) break;
        V.image.push_back(std::move(r));
    }
    fclose(f);
}

struct Input {
    std::string label;
    Cpu entry;
    bool recorded = false;
    std::vector<RecCall> calls;
    std::map<uint32_t, uint8_t> rec_net; /* recorded net writes outside the stack (replay check) */
    TapRegs rec_exit;
};

static const uint32_t kGenSp = 0x7EFFF000u;

static void base_cpu(Cpu* c) {
    memset(c, 0, sizeof *c);
    c->gqr[2] = 0x40004; c->gqr[3] = 0x50005; c->gqr[4] = 0x60006; c->gqr[5] = 0x70007;
    c->fpscr = 4;
    c->lr = 0x0BADC0DEu;
}

static void make_generated(const OrigFunc* of, uint64_t seed, Input* in) {
    char b[64];
    snprintf(b, sizeof b, "gen seed=%" PRIu64, seed);
    in->label = b;
    Cpu* c = &in->entry;
    base_cpu(c);
    c->r[1] = kGenSp;
    int fi = 1;
    for (int i = 3; i <= 10; i++) {
        uint64_t h = hash3(seed, i, 0xA4C);
        uint32_t x = (uint32_t)(h >> 16);
        switch (of->argtype[i]) {
        case 'p': c->r[i] = 0x40000000u + (uint32_t)(i - 3) * 0x01000000u + ((h % 4 == 0) ? 0 : 0); break;
        case 'h': c->r[i] = (uint32_t)(int32_t)(int16_t)x; break;
        case 'H': c->r[i] = x & 0xFFFF; break;
        case 'c': c->r[i] = (uint32_t)(int32_t)(int8_t)x; break;
        case 'b': c->r[i] = (h % 3 == 0) ? (x & 0xFF) : (x & 3); break;
        case 'i': c->r[i] = (h % 2) ? (x & 0xF) : x; break;
        default: c->r[i] = typed_word(h); break;
        }
    }
    for (; fi <= 8; fi++) c->f[fi].ps0 = c->f[fi].ps1 = rand_float(hash3(seed, fi, 0xF1));
    for (const FieldSpec& s : g_specs) {
        if (s.func && s.func != of->addr) continue;
        uint32_t a = (s.base_reg >= 0 ? c->r[s.base_reg] : 0) + s.off;
        uint64_t h = hash3(seed, a, 0xF1E1D);
        double v = s.lo + (double)(h % 1000003) / 1000003.0 * (s.hi - s.lo);
        uint64_t raw;
        if (s.is_float) raw = s.size == 4 ? f32_as_u32((float)v) : f64_as_u64(v);
        else raw = (uint64_t)(int64_t)(s.lo + (double)(h % (uint64_t)(s.hi - s.lo + 1)));
        if (h % 7 == 0 && !s.is_float) raw = (uint64_t)(int64_t)s.lo; /* edges */
        if (h % 7 == 1 && !s.is_float) raw = (uint64_t)(int64_t)s.hi;
        for (int k = 0; k < s.size; k++) snap_set(a + k, (uint8_t)(raw >> (8 * (s.size - 1 - k))));
    }
}

/* tap file -> snapshot + per-call results/patches */
static bool load_recorded(const char* path, Input* in, std::string* why) {
    FILE* f = fopen(path, "rb");
    if (!f) { *why = "cannot open"; return false; }
    TapHeader h;
    if (fread(&h, sizeof h, 1, f) != 1 || h.magic != TAP_MAGIC) { fclose(f); *why = "bad header"; return false; }
    in->label = path;
    in->recorded = true;
    base_cpu(&in->entry);
    tap_regs_to(&in->entry, &h.entry);
    std::unordered_map<uint32_t, uint8_t> shadow;
    std::vector<std::pair<uint32_t, uint8_t>> writes;
    uint32_t sp = in->entry.r[1];
    bool ok = true;
    for (;;) {
        TapEvent e;
        if (fread(&e, sizeof e, 1, f) != 1) { *why = "truncated"; ok = false; break; }
        if (e.type == TAP_READ || e.type == TAP_WRITE) {
            for (int k = 0; k < e.size; k++) {
                uint32_t a = e.ea + k;
                uint8_t v = (uint8_t)(e.value >> (8 * (e.size - 1 - k)));
                if (e.type == TAP_WRITE) { shadow[a] = v; writes.push_back({a, v}); continue; }
                auto it = shadow.find(a);
                bool own_frame = a - (sp - 0x10000) < 0x10000;
                if (it == shadow.end() && own_frame && !in->calls.empty()) {
                    /* first read of the function's own frame after a call: a callee's output */
                    in->calls.back().patches.push_back({a, v});
                    shadow[a] = v;
                } else if (it == shadow.end()) { snap_set(a, v); shadow[a] = v; }
                else if (it->second != v) {
                    if (in->calls.empty()) { *why = "memory changed without a call (another thread)"; ok = false; }
                    else in->calls.back().patches.push_back({a, v});
                    it->second = v;
                }
            }
        } else if (e.type == TAP_CALL) {
            RecCall rc;
            rc.target = e.ea;
            if (fread(&rc.at, sizeof rc.at, 1, f) != 1) { ok = false; break; }
            in->calls.push_back(rc);
        } else if (e.type == TAP_RET) {
            if (in->calls.empty() || fread(&in->calls.back().after, sizeof(TapRegs), 1, f) != 1) { ok = false; break; }
        } else if (e.type == TAP_END) {
            if (fread(&in->rec_exit, sizeof(TapRegs), 1, f) != 1) ok = false;
            break;
        } else { *why = "bad event"; ok = false; break; }
    }
    fclose(f);
    /* expected net writes: final shadow values that differ from the initial snapshot */
    for (auto& w : writes) {
        uint32_t a = w.first;
        if (a - (sp - 0x10000) < 0x10000 + 8) continue;
        uint8_t fin = shadow[a];
        bool known = snap_has(a) || image_region(a); /* same rule as net_writes() */
        uint8_t init = 0;
        if (known) init = initial_byte(a, &known);
        if (!known || init != fin) in->rec_net[a] = fin;
    }
    return ok;
}

/* ------------------------------------------------------------------ runs and comparison */
struct Result {
    uint32_t entry_sp = 0;
    Cpu exit;
    std::vector<CallEv> calls;
    std::map<uint32_t, uint8_t> net;
    bool aborted = false;
    std::string why;
    uint32_t stray = 0, first_stray = 0;
    int traps = 0;
};

static void run(void (*fn)(Cpu*), const Input& in, bool candidate, Result* out, std::vector<uint32_t>* cov) {
    reset_memory();
    V.calls.clear();
    V.per_target.clear();
    V.epoch = 0;
    V.depth = 0;
    V.is_candidate = candidate;
    V.budget = 0;
    V.stray = 0;
    V.traps = 0;
    V.cov = cov;
    V.mode = in.recorded ? MODE_REC : MODE_GEN;
    V.rec_calls = in.calls;
    V.entry_sp = in.entry.r[1];
    V.stack_lo = in.entry.r[1] - 0x10000;
    Cpu c = in.entry;
    gabi::cpu = &c;
    V.can_abort = true;
    if (setjmp(V.abort_jmp) == 0) {
        fn(&c);
    } else {
        out->aborted = true;
        out->why = V.abort_reason;
    }
    V.can_abort = false;
    V.cov = nullptr;
    out->exit = c;
    out->entry_sp = in.entry.r[1];
    out->calls = V.calls;
    out->net = net_writes();
    out->stray = V.stray;
    out->first_stray = V.first_stray;
    out->traps = V.traps;
}

static std::string hex32(uint32_t v) { char b[16]; snprintf(b, sizeof b, "%08X", v); return b; }

static std::string callee_name(uint32_t t) {
    const CalleeInfo* ci = callee_info(t);
    return hex32(t) + (ci && ci->name[0] ? std::string(" ") + ci->name : "");
}

/* NaN payloads are not part of the specification: when two NaNs meet, which one propagates
 * depends on the host compiler's operand order (in the recompiled original as much as in the
 * candidate), and clang may fold a float load/store pair that would quiet a signalling NaN.
 * So any two NaNs compare equal (-strictnan: bit-exact). */
static bool g_strict_nan = false;
static bool is_nan32(uint32_t w) { return (w & 0x7F800000u) == 0x7F800000u && (w & 0x007FFFFFu); }
static bool same_f(double a, double b) {
    if (!g_strict_nan && std::isnan(a) && std::isnan(b)) return true;
    return f64_as_u64(a) == f64_as_u64(b);
}

/* net memory effects equal, up to NaN payloads of aligned float words */
static bool same_net(const std::map<uint32_t, uint8_t>& x, const std::map<uint32_t, uint8_t>& y, uint32_t* where) {
    if (x == y) return true;
    std::map<uint32_t, int> words; /* aligned words with a difference */
    for (auto& kv : x) { auto it = y.find(kv.first); if (it == y.end() || it->second != kv.second) words[kv.first & ~3u] = 1; }
    for (auto& kv : y) { auto it = x.find(kv.first); if (it == x.end() || it->second != kv.second) words[kv.first & ~3u] = 1; }
    for (auto& w : words) {
        uint32_t wx = 0, wy = 0;
        for (uint32_t b = 0; b < 4; b++) {
            bool known;
            uint8_t init = initial_byte(w.first + b, &known);
            auto ix = x.find(w.first + b), iy = y.find(w.first + b);
            wx = wx << 8 | (ix != x.end() ? ix->second : init);
            wy = wy << 8 | (iy != y.end() ? iy->second : init);
        }
        if (wx != wy && !(!g_strict_nan && is_nan32(wx) && is_nan32(wy))) {
            *where = w.first;
            return false;
        }
    }
    return true;
}

/* stack objects passed by pointer: same bytes, except that pointers into the stack (e.g. an
 * object pointing at its own members) are compared relative to the object's address */
static bool same_stack_obj(const std::vector<uint8_t>& x, uint32_t bx, uint32_t spx, const std::vector<uint8_t>& y, uint32_t by,
                           uint32_t spy) {
    if (x.size() != y.size()) return false;
    for (size_t i = 0; i < x.size(); i += 4) {
        if (i + 4 > x.size()) return memcmp(&x[i], &y[i], x.size() - i) == 0;
        uint32_t wx = (uint32_t)x[i] << 24 | x[i + 1] << 16 | x[i + 2] << 8 | x[i + 3];
        uint32_t wy = (uint32_t)y[i] << 24 | y[i + 1] << 16 | y[i + 2] << 8 | y[i + 3];
        bool px = wx - (spx - 0x10000) < 0x10000, py = wy - (spy - 0x10000) < 0x10000;
        if (px && py) {
            if (wx - bx != wy - by) return false;
        } else if (wx != wy) {
            return false;
        }
    }
    return true;
}

/* first difference between original (a) and candidate (b), "" if equivalent */
static std::string compare(const Result& a, const Result& b, gabi::RetKind rk, const Input& in) {
    char buf[512];
    switch (rk) {
    case gabi::RET_VOID: break;
    case gabi::RET_INT1:
    case gabi::RET_INT2:
    case gabi::RET_INT4: {
        uint32_t m = rk == gabi::RET_INT1 ? 0xFF : rk == gabi::RET_INT2 ? 0xFFFF : 0xFFFFFFFF;
        if ((a.exit.r[3] & m) != (b.exit.r[3] & m)) {
            snprintf(buf, sizeof buf, "return r3: original %08X candidate %08X", a.exit.r[3], b.exit.r[3]);
            return buf;
        }
        break;
    }
    case gabi::RET_FLOAT:
        if (!same_f(a.exit.f[1].ps0, b.exit.f[1].ps0)) {
            snprintf(buf, sizeof buf, "return f1: original %.9g candidate %.9g", a.exit.f[1].ps0, b.exit.f[1].ps0);
            return buf;
        }
        break;
    }
    size_t n = std::min(a.calls.size(), b.calls.size());
    for (size_t k = 0; k < n; k++) {
        const CallEv& x = a.calls[k];
        const CallEv& y = b.calls[k];
        if (x.target != y.target) {
            snprintf(buf, sizeof buf, "call #%zu: original calls %s, candidate %s", k, callee_name(x.target).c_str(),
                     callee_name(y.target).c_str());
            return buf;
        }
        const CalleeInfo* ci = callee_info(x.target);
        uint32_t im = 0, fm = 0;
        if (ci) { im = ci->imask; fm = ci->fmask; }
        if (!ci || ci->declared) {
            for (int i = 0; i < y.nint; i++) im |= 1u << (3 + i);
            for (int i = 0; i < y.nflt; i++) fm |= 1u << (1 + i);
        }
        for (int i = 3; i <= 10; i++) {
            if (!(im >> i & 1)) continue;
            bool sa = !x.sref[i].empty(), sb = !y.sref[i].empty();
            if (sa && sb) {
                if (!same_stack_obj(x.sref[i], x.r[i], a.entry_sp, y.sref[i], y.r[i], b.entry_sp)) {
                    snprintf(buf, sizeof buf, "call #%zu %s: r%d -> stack data differs", k, callee_name(x.target).c_str(), i);
                    return buf;
                }
            } else if (x.r[i] != y.r[i]) {
                snprintf(buf, sizeof buf, "call #%zu %s: r%d original %08X candidate %08X", k, callee_name(x.target).c_str(), i,
                         x.r[i], y.r[i]);
                return buf;
            }
        }
        for (size_t j = 0; j < std::min(x.stk.size(), y.stk.size()); j++) {
            bool sa = !x.stkref[j].empty(), sb = !y.stkref[j].empty();
            if (sa && sb ? x.stkref[j] != y.stkref[j] : x.stk[j] != y.stk[j]) {
                snprintf(buf, sizeof buf, "call #%zu %s: stack argument %zu original %08X candidate %08X", k,
                         callee_name(x.target).c_str(), j, x.stk[j], y.stk[j]);
                return buf;
            }
        }
        for (int i = 1; i <= 8; i++) {
            if (!(fm >> i & 1)) continue;
            if (!same_f(x.f[i], y.f[i])) {
                snprintf(buf, sizeof buf, "call #%zu %s: f%d original %.9g candidate %.9g", k, callee_name(x.target).c_str(), i,
                         x.f[i], y.f[i]);
                return buf;
            }
        }
    }
    if (a.calls.size() != b.calls.size()) {
        const CallEv& e = a.calls.size() > n ? a.calls[n] : b.calls[n];
        snprintf(buf, sizeof buf, "call count: original %zu, candidate %zu (first extra: %s by the %s)", a.calls.size(),
                 b.calls.size(), callee_name(e.target).c_str(), a.calls.size() > n ? "original" : "candidate");
        return buf;
    }
    uint32_t wdiff = 0;
    if (!same_net(a.net, b.net, &wdiff)) {
        uint32_t ea = wdiff;
        for (uint32_t k = 0; k < 4; k++) {
            auto fa = a.net.find(wdiff + k), fb = b.net.find(wdiff + k);
            bool sa = fa != a.net.end(), sb = fb != b.net.end();
            if (sa != sb || (sa && fa->second != fb->second)) { ea = wdiff + k; break; }
        }
        auto fa = a.net.find(ea), fb = b.net.find(ea);
        snprintf(buf, sizeof buf, "memory %08X: original %s, candidate %s (%zu vs %zu bytes changed)", ea,
                 fa == a.net.end() ? "unchanged" : hex32(fa->second).substr(6).c_str(),
                 fb == b.net.end() ? "unchanged" : hex32(fb->second).substr(6).c_str(), a.net.size(), b.net.size());
        return buf;
    }
    (void)in;
    return "";
}

/* the replay of the original must reproduce the recording, else the input is unusable */
static std::string replay_check(const Result& a, const Input& in) {
    char buf[256];
    if (a.stray) { snprintf(buf, sizeof buf, "replay read unrecorded memory (%u bytes, first %08X)", a.stray, a.first_stray); return buf; }
    if (a.calls.size() != in.calls.size()) { snprintf(buf, sizeof buf, "replay made %zu calls, recording %zu", a.calls.size(), in.calls.size()); return buf; }
    for (size_t k = 0; k < a.calls.size(); k++)
        if (a.calls[k].target != in.calls[k].target) { snprintf(buf, sizeof buf, "replay call #%zu differs", k); return buf; }
    if (a.net != in.rec_net) { snprintf(buf, sizeof buf, "replay writes differ from the recording (%zu vs %zu bytes)", a.net.size(), in.rec_net.size()); return buf; }
    if (a.exit.r[3] != in.rec_exit.r[3] || !same_f(a.exit.f[1].ps0, in.rec_exit.ps0[1])) return "replay return value differs";
    return "";
}

/* ------------------------------------------------------------------ main */
int main(int argc, char** argv) {
    int ngen = 1000, verbose = 0;
    uint64_t seed0 = 1;
    const char* recdir = nullptr;
    const char* image = "build/verify/image.bin";
    uint32_t only = 0;
    uint64_t trace_seed = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) ngen = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-seed") && i + 1 < argc) seed0 = strtoull(argv[++i], nullptr, 0);
        else if (!strcmp(argv[i], "-rec") && i + 1 < argc) recdir = argv[++i];
        else if (!strcmp(argv[i], "-image") && i + 1 < argc) image = argv[++i];
        else if (!strcmp(argv[i], "-only") && i + 1 < argc) only = (uint32_t)strtoul(argv[++i], nullptr, 16);
        else if (!strcmp(argv[i], "-spec") && i + 1 < argc) load_specs(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose++;
        else if (!strcmp(argv[i], "-noclobber")) V.clobber = false;
        else if (!strcmp(argv[i], "-strictnan")) g_strict_nan = true;
        else if (!strcmp(argv[i], "-trace") && i + 1 < argc) trace_seed = strtoull(argv[++i], nullptr, 0);
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    load_image(image);

    std::vector<gabi::Candidate*> cands;
    for (gabi::Candidate* c = gabi::g_cands; c; c = c->next) cands.push_back(c);
    std::sort(cands.begin(), cands.end(), [](auto* a, auto* b) { return a->addr < b->addr; });

    int total_fail = 0;
    printf("%-8s  %-40s %10s %10s %8s  %s\n", "addr", "function", "generated", "recorded", "coverage", "first difference");
    for (gabi::Candidate* cand : cands) {
        if (only && cand->addr != only) continue;
        const OrigFunc* of = nullptr;
        for (unsigned i = 0; i < unit_nfuncs; i++)
            if (unit_funcs[i].addr == cand->addr) of = &unit_funcs[i];
        if (!of) { printf("%08X  %-40s  no original in unit\n", cand->addr, cand->name); total_fail++; continue; }
        std::vector<uint32_t> cov(of->nblocks + 1);
        int gpass = 0, gn = 0, rpass = 0, rn = 0, rbad = 0, aborted = 0;
        std::string first;
        auto one = [&](Input& in, bool rec) {
            Result a, b;
            run(of->fn, in, false, &a, &cov);
            if (rec) {
                std::string why = replay_check(a, in);
                if (!why.empty()) {
                    rbad++;
                    if (verbose) printf("    unusable %s: %s\n", in.label.c_str(), why.c_str());
                    return;
                }
            }
            run(cand->fn, in, true, &b, nullptr);
            if (a.aborted || b.aborted) {
                if (a.aborted && b.aborted) { aborted++; return; }
            }
            std::string d = a.aborted != b.aborted ? std::string("only one side aborted: ") + (a.aborted ? a.why : b.why)
                                                   : compare(a, b, cand->ret, in);
            if (!d.empty() && b.stray && rec) d += " [candidate read unrecorded memory]";
            (rec ? rn : gn)++;
            if (d.empty()) (rec ? rpass : gpass)++;
            else {
                if (first.empty()) first = d + "  {" + in.label + "}";
                if (verbose) printf("    FAIL %s: %s\n", in.label.c_str(), d.c_str());
                if (verbose > 2) { /* full net effects of both sides */
                    for (auto& kv : a.net) printf("      orig %08X=%02X%s\n", kv.first, kv.second, b.net.count(kv.first) && b.net.at(kv.first) == kv.second ? "" : "  <");
                    for (auto& kv : b.net) printf("      cand %08X=%02X%s\n", kv.first, kv.second, a.net.count(kv.first) && a.net.at(kv.first) == kv.second ? "" : "  <");
                }
            }
        };
        /* dictionary of the original's .rodata constants (pre-runs, not compared) */
        V.dict.clear();
        g_dict = nullptr;
        V.collect = true;
        for (int i = 0; i < 8 && ngen; i++) {
            clear_all();
            Input in;
            make_generated(of, seed0 * 7777ull + i, &in);
            V.seed = seed0 * 7777ull + i;
            Result a;
            run(of->fn, in, false, &a, nullptr);
        }
        V.collect = false;
        g_dict = &V.dict;
        for (int i = 0; i < ngen; i++) {
            clear_all();
            Input in;
            make_generated(of, seed0 * 1000003ull + i, &in);
            V.seed = seed0 * 1000003ull + i;
            V.clobber_now = i % 2 == 0;
            g_trace = V.seed == trace_seed;
            if (g_trace) printf("    trace %s (clobber %d)\n", in.label.c_str(), V.clobber_now);
            one(in, false);
            g_trace = 0;
        }
        g_dict = nullptr;
        if (recdir) {
            std::string d = std::string(recdir) + "/" + hex32(cand->addr);
            if (DIR* dir = opendir(d.c_str())) {
                std::vector<std::string> files;
                while (dirent* e = readdir(dir))
                    if (strstr(e->d_name, ".tap")) files.push_back(d + "/" + e->d_name);
                closedir(dir);
                std::sort(files.begin(), files.end());
                for (auto& fpath : files) {
                    clear_all();
                    Input in;
                    std::string why;
                    V.seed = 77;
                    if (!load_recorded(fpath.c_str(), &in, &why)) {
                        rbad++;
                        if (verbose) printf("    unusable %s: %s\n", fpath.c_str(), why.c_str());
                        continue;
                    }
                    one(in, true);
                }
            }
        }
        uint32_t hit = 0;
        for (uint32_t k = 0; k < of->nblocks; k++) hit += cov[k] > 0;
        char g[32], r[32], cv[32];
        snprintf(g, sizeof g, "%d/%d", gpass, gn);
        snprintf(r, sizeof r, rbad ? "%d/%d(-%d)" : "%d/%d", rpass, rn, rbad);
        snprintf(cv, sizeof cv, "%u/%u", hit, of->nblocks);
        bool ok = gpass == gn && rpass == rn && (gn + rn) > 0;
        if (!ok) total_fail++;
        printf("%08X  %-40.40s %10s %10s %8s  %s%s\n", cand->addr, cand->name, g, r, cv, ok ? "ok" : first.c_str(),
               aborted ? " (some inputs aborted on both sides)" : "");
        if (verbose > 1) {
            printf("    uncovered blocks:");
            for (uint32_t k = 0; k < of->nblocks; k++) if (!cov[k]) printf(" %u", k);
            printf("\n");
        }
    }
    return total_fail ? 1 : 0;
}
