/* Semantics of the Espresso instructions that don't change control flow, one function per
 * instruction form, for the on-device recompiler (recomp/emit.cpp).
 *
 * Each function decodes its operands from the instruction word `w` itself. The recompiler emits a
 * call per guest instruction with `w` (and `addr`) as constants; after inlining, LLVM folds the
 * decoding away, so the result is the same as tools/recomp/ppc2c.py's C (which this is a port
 * of; semantics follow Cemu's interpreter). This file is compiled to LLVM bitcode at APK build
 * time (runtime code, no game code) and natively for ppcop_select().
 *
 * Branches, calls, returns and indirect jumps are handled by the recompiler, not here.
 */
#include "ppc.h"

#define OP(name) __attribute__((always_inline)) void ppcop_##name(Cpu* __restrict c, uint32_t w, uint32_t addr)
#define D ((w >> 21) & 31)
#define A ((w >> 16) & 31)
#define B ((w >> 11) & 31)
#define CC ((w >> 6) & 31)
#define SIMM ((uint32_t)(int32_t)(int16_t)(w & 0xFFFF))
#define UIMM (w & 0xFFFFu)
#define RA0 (A ? c->r[A] : 0u)
#define RC (w & 1)
#define OE ((w >> 10) & 1)
#define F0(n) c->f[n].ps0
#define F1(n) c->f[n].ps1
#define UNUSED (void)addr

static inline void rc0(Cpu* c, uint32_t w, uint32_t v) { if (w & 1) cr0_rc(c, v); }
static inline void fprc(Cpu* c, uint32_t w) { if (w & 1) c->cr[4] = c->cr[5] = c->cr[6] = c->cr[7] = 0; }
static inline uint32_t ppc_mask(uint32_t mb, uint32_t me) {
    uint32_t m = 0;
    for (uint32_t i = mb;; i = (i + 1) & 31) {
        m |= 1u << (31 - i);
        if (i == me) break;
    }
    return m;
}
static inline void set_ov(Cpu* c, uint8_t o) { c->xer_ov = o; c->xer_so |= o; }
static inline void trap_if(Cpu* c, uint32_t to, uint32_t x, uint32_t y, uint32_t addr) {
    if (to == 31) { ppc_trap(c, addr); return; }
    if (((to & 16) && (int32_t)x < (int32_t)y) || ((to & 8) && (int32_t)x > (int32_t)y) || ((to & 4) && x == y) ||
        ((to & 2) && x < y) || ((to & 1) && x > y))
        ppc_trap(c, addr);
}

/* function entry: trace and preemption check (PPC_ENTER in ppc.h); `addr` is the function */
OP(enter) { (void)w; PPC_ENTER(addr); }

/* ---- condition register (op 19) ---- */
OP(crand) { UNUSED; c->cr[D] = c->cr[A] & c->cr[B]; }
OP(cror) { UNUSED; c->cr[D] = c->cr[A] | c->cr[B]; }
OP(crxor) { UNUSED; c->cr[D] = c->cr[A] ^ c->cr[B]; }
OP(crnand) { UNUSED; c->cr[D] = !(c->cr[A] & c->cr[B]); }
OP(crnor) { UNUSED; c->cr[D] = !(c->cr[A] | c->cr[B]); }
OP(creqv) { UNUSED; c->cr[D] = !(c->cr[A] ^ c->cr[B]); }
OP(crandc) { UNUSED; c->cr[D] = c->cr[A] & !c->cr[B]; }
OP(crorc) { UNUSED; c->cr[D] = c->cr[A] | !c->cr[B]; }
OP(mcrf) { UNUSED; memmove(&c->cr[4 * (D >> 2)], &c->cr[4 * (A >> 2)], 4); }
OP(isync) { UNUSED; (void)c; (void)w; __atomic_thread_fence(__ATOMIC_ACQUIRE); }

/* ---- integer immediate ---- */
OP(addi) { UNUSED; c->r[D] = RA0 + SIMM; }
OP(addis) { UNUSED; c->r[D] = RA0 + (UIMM << 16); }
OP(addic) { UNUSED; uint64_t t = (uint64_t)c->r[A] + SIMM; c->r[D] = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); if ((w >> 26) == 13) cr0_rc(c, c->r[D]); }
OP(subfic) { UNUSED; uint64_t t = (uint64_t)(uint32_t)~c->r[A] + SIMM + 1; c->r[D] = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); }
/* unsigned: the same low 32 bits without signed overflow, which is undefined in C and lets the
   compiler assume it never happens; the game overflows on purpose (original project defb89f) */
OP(mulli) { UNUSED; c->r[D] = c->r[A] * SIMM; }
OP(cmpli) { UNUSED; cr_set_u(c, D >> 2, c->r[A], UIMM); }
OP(cmpi) { UNUSED; cr_set_s(c, D >> 2, (int32_t)c->r[A], (int32_t)SIMM); }
OP(ori) { UNUSED; c->r[A] = c->r[D] | UIMM; }
OP(oris) { UNUSED; c->r[A] = c->r[D] | (UIMM << 16); }
OP(xori) { UNUSED; c->r[A] = c->r[D] ^ UIMM; }
OP(xoris) { UNUSED; c->r[A] = c->r[D] ^ (UIMM << 16); }
OP(andi) { UNUSED; c->r[A] = c->r[D] & UIMM; cr0_rc(c, c->r[A]); }
OP(andis) { UNUSED; c->r[A] = c->r[D] & (UIMM << 16); cr0_rc(c, c->r[A]); }

/* ---- rotates ---- */
OP(rlwinm) { UNUSED; c->r[A] = rotl32(c->r[D], B) & ppc_mask(CC, (w >> 1) & 31); rc0(c, w, c->r[A]); }
OP(rlwimi) { UNUSED; uint32_t m = ppc_mask(CC, (w >> 1) & 31); c->r[A] = (rotl32(c->r[D], B) & m) | (c->r[A] & ~m); rc0(c, w, c->r[A]); }
OP(rlwnm) { UNUSED; c->r[A] = rotl32(c->r[D], c->r[B] & 31) & ppc_mask(CC, (w >> 1) & 31); rc0(c, w, c->r[A]); }

/* ---- integer loads / stores, D-form ---- */
OP(lwz) { UNUSED; c->r[D] = ld32(RA0 + SIMM); }
OP(lwzu) { UNUSED; uint32_t ea = c->r[A] + SIMM; c->r[D] = ld32(ea); c->r[A] = ea; }
OP(lbz) { UNUSED; c->r[D] = ld8(RA0 + SIMM); }
OP(lbzu) { UNUSED; uint32_t ea = c->r[A] + SIMM; c->r[D] = ld8(ea); c->r[A] = ea; }
OP(lhz) { UNUSED; c->r[D] = ld16(RA0 + SIMM); }
OP(lhzu) { UNUSED; uint32_t ea = c->r[A] + SIMM; c->r[D] = ld16(ea); c->r[A] = ea; }
OP(lha) { UNUSED; c->r[D] = (uint32_t)(int32_t)(int16_t)ld16(RA0 + SIMM); }
OP(lhau) { UNUSED; uint32_t ea = c->r[A] + SIMM; c->r[D] = (uint32_t)(int32_t)(int16_t)ld16(ea); c->r[A] = ea; }
OP(stw) { UNUSED; st32(RA0 + SIMM, c->r[D]); }
OP(stwu) { UNUSED; uint32_t ea = c->r[A] + SIMM; st32(ea, c->r[D]); c->r[A] = ea; }
OP(stb) { UNUSED; st8(RA0 + SIMM, (uint8_t)c->r[D]); }
OP(stbu) { UNUSED; uint32_t ea = c->r[A] + SIMM; st8(ea, (uint8_t)c->r[D]); c->r[A] = ea; }
OP(sth) { UNUSED; st16(RA0 + SIMM, (uint16_t)c->r[D]); }
OP(sthu) { UNUSED; uint32_t ea = c->r[A] + SIMM; st16(ea, (uint16_t)c->r[D]); c->r[A] = ea; }
OP(lmw) { UNUSED; uint32_t ea = RA0 + SIMM; for (uint32_t r = D; r < 32; r++) c->r[r] = ld32(ea + 4 * (r - D)); }
OP(stmw) { UNUSED; uint32_t ea = RA0 + SIMM; for (uint32_t r = D; r < 32; r++) st32(ea + 4 * (r - D), c->r[r]); }

/* ---- floating point loads / stores, D-form ---- */
OP(lfs) { UNUSED; double v = ldf32(RA0 + SIMM); F0(D) = v; F1(D) = v; }
OP(lfsu) { UNUSED; uint32_t ea = c->r[A] + SIMM; double v = ldf32(ea); F0(D) = v; F1(D) = v; c->r[A] = ea; }
OP(lfd) { UNUSED; F0(D) = ldf64(RA0 + SIMM); }
OP(lfdu) { UNUSED; uint32_t ea = c->r[A] + SIMM; F0(D) = ldf64(ea); c->r[A] = ea; }
OP(stfs) { UNUSED; stf32(RA0 + SIMM, F0(D)); }
OP(stfsu) { UNUSED; uint32_t ea = c->r[A] + SIMM; stf32(ea, F0(D)); c->r[A] = ea; }
OP(stfd) { UNUSED; stf64(RA0 + SIMM, F0(D)); }
OP(stfdu) { UNUSED; uint32_t ea = c->r[A] + SIMM; stf64(ea, F0(D)); c->r[A] = ea; }
#define PSQ_OFF ((uint32_t)((int32_t)(w << 20) >> 20))
OP(psq_l) { UNUSED; psq_load(c, D, RA0 + PSQ_OFF, (w >> 15) & 1, (w >> 12) & 7); }
OP(psq_lu) { UNUSED; uint32_t ea = c->r[A] + PSQ_OFF; psq_load(c, D, ea, (w >> 15) & 1, (w >> 12) & 7); c->r[A] = ea; }
OP(psq_st) { UNUSED; psq_store(c, D, RA0 + PSQ_OFF, (w >> 15) & 1, (w >> 12) & 7); }
OP(psq_stu) { UNUSED; uint32_t ea = c->r[A] + PSQ_OFF; psq_store(c, D, ea, (w >> 15) & 1, (w >> 12) & 7); c->r[A] = ea; }

/* ---- traps, system call ---- */
OP(twi) { trap_if(c, D, c->r[A], SIMM, addr); }
OP(tw) { trap_if(c, D, c->r[A], c->r[B], addr); }
OP(unimplemented) { ppc_unimplemented(c, addr, w); }

/* ---- op 31: XO-form arithmetic ---- */
OP(add) {
    UNUSED;
    uint32_t x = c->r[A], y = c->r[B], r = x + y;
    c->r[D] = r;
    if (OE) set_ov(c, (uint8_t)((~(x ^ y) & (x ^ r)) >> 31));
    rc0(c, w, r);
}
OP(subf) {
    UNUSED;
    uint32_t x = ~c->r[A], y = c->r[B], r = c->r[B] - c->r[A];
    c->r[D] = r;
    if (OE) set_ov(c, (uint8_t)((~(x ^ y) & (x ^ r)) >> 31));
    rc0(c, w, r);
}
#define CARRY(expr) do { uint64_t t = (expr); c->r[D] = (uint32_t)t; c->xer_ca = (uint8_t)(t >> 32); rc0(c, w, c->r[D]); } while (0)
OP(addc) { UNUSED; CARRY((uint64_t)c->r[A] + c->r[B]); }
OP(adde) { UNUSED; CARRY((uint64_t)c->r[A] + c->r[B] + c->xer_ca); }
OP(subfc) { UNUSED; CARRY((uint64_t)(uint32_t)~c->r[A] + c->r[B] + 1); }
OP(subfe) { UNUSED; CARRY((uint64_t)(uint32_t)~c->r[A] + c->r[B] + c->xer_ca); }
OP(addme) { UNUSED; CARRY((uint64_t)c->r[A] + c->xer_ca + 0xFFFFFFFFu); }
OP(addze) { UNUSED; CARRY((uint64_t)c->r[A] + c->xer_ca); }
OP(subfme) { UNUSED; CARRY((uint64_t)(uint32_t)~c->r[A] + c->xer_ca + 0xFFFFFFFFu); }
OP(subfze) { UNUSED; CARRY((uint64_t)(uint32_t)~c->r[A] + c->xer_ca); }
OP(neg) {
    UNUSED;
    uint32_t a = c->r[A];
    if (OE) set_ov(c, a == 0x80000000u);
    c->r[D] = 0u - a;
    rc0(c, w, c->r[D]);
}
OP(mullw) {
    UNUSED;
    int64_t t = (int64_t)(int32_t)c->r[A] * (int32_t)c->r[B];
    c->r[D] = (uint32_t)t;
    if (OE) set_ov(c, t != (int32_t)t);
    rc0(c, w, c->r[D]);
}
OP(mulhw) { UNUSED; c->r[D] = (uint32_t)(((int64_t)(int32_t)c->r[A] * (int32_t)c->r[B]) >> 32); rc0(c, w, c->r[D]); }
OP(mulhwu) { UNUSED; c->r[D] = (uint32_t)(((uint64_t)c->r[A] * c->r[B]) >> 32); rc0(c, w, c->r[D]); }
OP(divw) {
    UNUSED;
    uint32_t a = c->r[A], b = c->r[B];
    if (OE) set_ov(c, b == 0 || (a == 0x80000000u && b == 0xFFFFFFFFu));
    c->r[D] = ppc_divw(a, b);
    rc0(c, w, c->r[D]);
}
OP(divwu) {
    UNUSED;
    uint32_t a = c->r[A], b = c->r[B];
    if (OE) set_ov(c, b == 0);
    c->r[D] = ppc_divwu(a, b);
    rc0(c, w, c->r[D]);
}

/* ---- op 31: logical, shifts, extends (rS = D) ---- */
OP(and) { UNUSED; c->r[A] = c->r[D] & c->r[B]; rc0(c, w, c->r[A]); }
OP(or) { UNUSED; c->r[A] = c->r[D] | c->r[B]; rc0(c, w, c->r[A]); }
OP(xor) { UNUSED; c->r[A] = c->r[D] ^ c->r[B]; rc0(c, w, c->r[A]); }
OP(andc) { UNUSED; c->r[A] = c->r[D] & ~c->r[B]; rc0(c, w, c->r[A]); }
OP(orc) { UNUSED; c->r[A] = c->r[D] | ~c->r[B]; rc0(c, w, c->r[A]); }
OP(nand) { UNUSED; c->r[A] = ~(c->r[D] & c->r[B]); rc0(c, w, c->r[A]); }
OP(nor) { UNUSED; c->r[A] = ~(c->r[D] | c->r[B]); rc0(c, w, c->r[A]); }
OP(eqv) { UNUSED; c->r[A] = ~(c->r[D] ^ c->r[B]); rc0(c, w, c->r[A]); }
OP(slw) { UNUSED; uint32_t n = c->r[B]; c->r[A] = (n & 0x20) ? 0 : c->r[D] << (n & 31); rc0(c, w, c->r[A]); }
OP(srw) { UNUSED; uint32_t n = c->r[B]; c->r[A] = (n & 0x20) ? 0 : c->r[D] >> (n & 31); rc0(c, w, c->r[A]); }
OP(sraw) {
    UNUSED;
    uint32_t n = c->r[B] & 0x3F;
    int32_t s = (int32_t)c->r[D];
    if (n > 31) { c->r[A] = (uint32_t)(s >> 31); c->xer_ca = s < 0; }
    else { c->r[A] = (uint32_t)(s >> n); c->xer_ca = s < 0 && n && ((uint32_t)s << (32 - n)) != 0; }
    rc0(c, w, c->r[A]);
}
OP(srawi) {
    UNUSED;
    uint32_t sh = B;
    int32_t s = (int32_t)c->r[D];
    if (sh == 0) { c->r[A] = (uint32_t)s; c->xer_ca = 0; }
    else { c->xer_ca = s < 0 && (s & ((1u << sh) - 1)); c->r[A] = (uint32_t)(s >> sh); }
    rc0(c, w, c->r[A]);
}
OP(cntlzw) { UNUSED; uint32_t s = c->r[D]; c->r[A] = s ? (uint32_t)__builtin_clz(s) : 32; rc0(c, w, c->r[A]); }
OP(extsb) { UNUSED; c->r[A] = (uint32_t)(int32_t)(int8_t)c->r[D]; rc0(c, w, c->r[A]); }
OP(extsh) { UNUSED; c->r[A] = (uint32_t)(int32_t)(int16_t)c->r[D]; rc0(c, w, c->r[A]); }
OP(cmp) { UNUSED; cr_set_s(c, D >> 2, (int32_t)c->r[A], (int32_t)c->r[B]); }
OP(cmpl) { UNUSED; cr_set_u(c, D >> 2, c->r[A], c->r[B]); }

/* ---- op 31: indexed loads / stores ---- */
#define EAX (RA0 + c->r[B])
OP(lwzx) { UNUSED; c->r[D] = ld32(EAX); }
OP(lbzx) { UNUSED; c->r[D] = ld8(EAX); }
OP(lhzx) { UNUSED; c->r[D] = ld16(EAX); }
OP(lhax) { UNUSED; c->r[D] = (uint32_t)(int32_t)(int16_t)ld16(EAX); }
OP(lwzux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; c->r[D] = ld32(ea); c->r[A] = ea; }
OP(lbzux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; c->r[D] = ld8(ea); c->r[A] = ea; }
OP(lhzux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; c->r[D] = ld16(ea); c->r[A] = ea; }
OP(lhaux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; c->r[D] = (uint32_t)(int32_t)(int16_t)ld16(ea); c->r[A] = ea; }
OP(stwx) { UNUSED; st32(EAX, c->r[D]); }
OP(stbx) { UNUSED; st8(EAX, (uint8_t)c->r[D]); }
OP(sthx) { UNUSED; st16(EAX, (uint16_t)c->r[D]); }
OP(stwux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; st32(ea, c->r[D]); c->r[A] = ea; }
OP(stbux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; st8(ea, (uint8_t)c->r[D]); c->r[A] = ea; }
OP(sthux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; st16(ea, (uint16_t)c->r[D]); c->r[A] = ea; }
OP(lwbrx) { UNUSED; c->r[D] = __builtin_bswap32(ld32(EAX)); }
OP(lhbrx) { UNUSED; c->r[D] = __builtin_bswap16(ld16(EAX)); }
OP(stwbrx) { UNUSED; st32(EAX, __builtin_bswap32(c->r[D])); }
OP(sthbrx) { UNUSED; st16(EAX, __builtin_bswap16((uint16_t)c->r[D])); }
OP(lwarx) { UNUSED; c->r[D] = ppc_lwarx(c, EAX); }
OP(stwcx) { UNUSED; ppc_stwcx(c, EAX, c->r[D]); }
OP(lswi) {
    UNUSED;
    uint32_t n = B ? B : 32, ea = RA0, r = (D - 1) & 31;
    for (uint32_t i = 0; i < n; i++) {
        if (i % 4 == 0) { r = (r + 1) & 31; c->r[r] = 0; }
        c->r[r] |= (uint32_t)ld8(ea + i) << (24 - 8 * (i % 4));
    }
}
OP(stswi) {
    UNUSED;
    uint32_t n = B ? B : 32, ea = RA0, r = (D - 1) & 31;
    for (uint32_t i = 0; i < n; i++) {
        if (i % 4 == 0) r = (r + 1) & 31;
        st8(ea + i, (uint8_t)(c->r[r] >> (24 - 8 * (i % 4))));
    }
}
OP(lfsx) { UNUSED; double v = ldf32(EAX); F0(D) = v; F1(D) = v; }
OP(lfsux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; double v = ldf32(ea); F0(D) = v; F1(D) = v; c->r[A] = ea; }
OP(lfdx) { UNUSED; F0(D) = ldf64(EAX); }
OP(lfdux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; F0(D) = ldf64(ea); c->r[A] = ea; }
OP(stfsx) { UNUSED; stf32(EAX, F0(D)); }
OP(stfsux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; stf32(ea, F0(D)); c->r[A] = ea; }
OP(stfdx) { UNUSED; stf64(EAX, F0(D)); }
OP(stfdux) { UNUSED; uint32_t ea = c->r[A] + c->r[B]; stf64(ea, F0(D)); c->r[A] = ea; }
OP(stfiwx) { UNUSED; st32(EAX, (uint32_t)f64_as_u64(F0(D))); }

/* cache ops are no-ops except dcbz; sync/eieio are real barriers (see ppc2c.py) */
OP(dcbz) { UNUSED; ppc_dcbz(EAX); }
OP(sync) { UNUSED; (void)c; (void)w; __atomic_thread_fence(__ATOMIC_SEQ_CST); }
OP(nop) { UNUSED; (void)c; (void)w; }

/* ---- op 31: special registers ---- */
#define SPR (((w >> 16) & 0x1F) | (((w >> 11) & 0x1F) << 5))
OP(mfspr) {
    UNUSED;
    uint32_t spr = SPR, v = 0;
    if (spr == 8) v = c->lr;
    else if (spr == 9) v = c->ctr;
    else if (spr == 1) v = ppc_mfxer(c);
    else if (spr >= 912 && spr <= 919) v = c->gqr[spr - 912];
    else if (spr >= 896 && spr <= 903) v = c->gqr[spr - 896];
    else if (spr == 268 || spr == 269) v = (uint32_t)(ppc_timebase() >> (spr == 268 ? 0 : 32));
    c->r[D] = v;
}
OP(mtspr) {
    UNUSED;
    uint32_t spr = SPR, v = c->r[D];
    if (spr == 8) c->lr = v;
    else if (spr == 9) c->ctr = v;
    else if (spr == 1) ppc_mtxer(c, v);
    else if (spr >= 912 && spr <= 919) c->gqr[spr - 912] = v;
    else if (spr >= 896 && spr <= 903) c->gqr[spr - 896] = v;
}
OP(mftb) { UNUSED; c->r[D] = (uint32_t)(ppc_timebase() >> (SPR == 268 ? 0 : 32)); }
OP(mfcr) { UNUSED; c->r[D] = ppc_mfcr(c); }
OP(mtcrf) { UNUSED; ppc_mtcrf(c, (w >> 12) & 0xFF, c->r[D]); }
OP(mcrxr) {
    UNUSED;
    uint32_t f = D >> 2;
    c->cr[4 * f] = c->xer_so; c->cr[4 * f + 1] = c->xer_ov; c->cr[4 * f + 2] = c->xer_ca; c->cr[4 * f + 3] = 0;
    c->xer_so = c->xer_ov = c->xer_ca = 0;
}

/* ---- op 59: single precision ---- */
#define SET_SINGLE(expr) do { double v = to_single(expr); F0(D) = v; F1(D) = v; fprc(c, w); } while (0)
OP(fdivs) { UNUSED; SET_SINGLE(F0(A) / F0(B)); }
OP(fsubs) { UNUSED; SET_SINGLE(F0(A) - F0(B)); }
OP(fadds) { UNUSED; SET_SINGLE(F0(A) + F0(B)); }
OP(fmuls) { UNUSED; SET_SINGLE(F0(A) * round25(F0(CC))); }
OP(fmsubs) { UNUSED; SET_SINGLE(F0(A) * round25(F0(CC)) - F0(B)); }
OP(fmadds) { UNUSED; SET_SINGLE(F0(A) * round25(F0(CC)) + F0(B)); }
OP(fnmsubs) { UNUSED; SET_SINGLE(-(F0(A) * round25(F0(CC)) - F0(B))); }
OP(fnmadds) { UNUSED; SET_SINGLE(-(F0(A) * round25(F0(CC)) + F0(B))); }
OP(fres) { UNUSED; SET_SINGLE(ppc_fres(F0(B))); }

/* ---- op 63: double precision and FPSCR ---- */
#define SET_DOUBLE(expr) do { F0(D) = (expr); fprc(c, w); } while (0)
OP(fdiv) { UNUSED; SET_DOUBLE(F0(A) / F0(B)); }
OP(fsub) { UNUSED; SET_DOUBLE(F0(A) - F0(B)); }
OP(fadd) { UNUSED; SET_DOUBLE(F0(A) + F0(B)); }
OP(fsqrt) { UNUSED; SET_DOUBLE(sqrt(F0(B))); }
OP(fsel) { UNUSED; SET_DOUBLE(ppc_fsel(F0(A), F0(B), F0(CC))); }
OP(fmul) { UNUSED; SET_DOUBLE(F0(A) * F0(CC)); }
OP(frsqrte) { UNUSED; SET_DOUBLE(ppc_frsqrte(F0(B))); }
OP(fmsub) { UNUSED; SET_DOUBLE(fma(F0(A), F0(CC), -F0(B))); }
OP(fmadd) { UNUSED; SET_DOUBLE(fma(F0(A), F0(CC), F0(B))); }
OP(fnmsub) { UNUSED; SET_DOUBLE(-fma(F0(A), F0(CC), -F0(B))); }
OP(fnmadd) { UNUSED; SET_DOUBLE(-fma(F0(A), F0(CC), F0(B))); }
OP(fcmp) { UNUSED; cr_set_f(c, D >> 2, F0(A), F0(B)); }
OP(frsp) { UNUSED; SET_SINGLE(F0(B)); }
OP(fctiwz) { UNUSED; SET_DOUBLE(u64_as_f64(ppc_fctiwz(F0(B)))); }
OP(fctiw) { UNUSED; SET_DOUBLE(u64_as_f64(ppc_fctiw(c, F0(B)))); }
OP(fmr) { UNUSED; SET_DOUBLE(F0(B)); }
OP(fneg) { UNUSED; SET_DOUBLE(-F0(B)); }
OP(fabs) { UNUSED; SET_DOUBLE(fabs(F0(B))); }
OP(fnabs) { UNUSED; SET_DOUBLE(-fabs(F0(B))); }
OP(mffs) { UNUSED; F0(D) = u64_as_f64(0xFFF8000000000000ull | c->fpscr); }
OP(mtfsf) {
    UNUSED;
    uint32_t fm = (w >> 17) & 0xFF, m = 0;
    for (int i = 0; i < 8; i++)
        if (fm & (0x80u >> i)) m |= 0xF0000000u >> (4 * i);
    c->fpscr = (c->fpscr & ~m) | ((uint32_t)f64_as_u64(F0(B)) & m);
}
OP(mtfsfi) { UNUSED; uint32_t sh = 28 - 4 * (D >> 2); c->fpscr = (c->fpscr & ~(0xFu << sh)) | (((w >> 12) & 0xF) << sh); }
OP(mtfsb1) { UNUSED; c->fpscr |= 0x80000000u >> D; }
OP(mtfsb0) { UNUSED; c->fpscr &= ~(0x80000000u >> D); }
OP(mcrfs) {
    UNUSED;
    uint32_t fd = D >> 2, sh = 28 - 4 * (A >> 2), v = (c->fpscr >> sh) & 0xF;
    c->cr[4 * fd] = v >> 3; c->cr[4 * fd + 1] = (v >> 2) & 1; c->cr[4 * fd + 2] = (v >> 1) & 1; c->cr[4 * fd + 3] = v & 1;
}

/* ---- op 4: paired singles ---- */
#define PAIR(e0, e1) do { double v0 = to_single(e0), v1 = to_single(e1); F0(D) = v0; F1(D) = v1; fprc(c, w); } while (0)
OP(ps_sum0) { UNUSED; double v0 = to_single(F0(A) + F1(B)), v1 = F1(CC); F0(D) = v0; F1(D) = v1; }
OP(ps_sum1) { UNUSED; double v0 = F0(CC), v1 = to_single(F0(A) + F1(B)); F0(D) = v0; F1(D) = v1; }
OP(ps_muls0) { UNUSED; PAIR(F0(A) * round25(F0(CC)), F1(A) * round25(F0(CC))); }
OP(ps_muls1) { UNUSED; PAIR(F0(A) * round25(F1(CC)), F1(A) * round25(F1(CC))); }
OP(ps_madds0) { UNUSED; PAIR(F0(A) * round25(F0(CC)) + F0(B), F1(A) * round25(F0(CC)) + F1(B)); }
OP(ps_madds1) { UNUSED; PAIR(F0(A) * round25(F1(CC)) + F0(B), F1(A) * round25(F1(CC)) + F1(B)); }
OP(ps_div) { UNUSED; PAIR(F0(A) / F0(B), F1(A) / F1(B)); }
OP(ps_sub) { UNUSED; PAIR(F0(A) - F0(B), F1(A) - F1(B)); }
OP(ps_add) { UNUSED; PAIR(F0(A) + F0(B), F1(A) + F1(B)); }
OP(ps_sel) {
    UNUSED;
    double v0 = ppc_fsel(F0(A), F0(B), F0(CC)), v1 = ppc_fsel(F1(A), F1(B), F1(CC));
    F0(D) = v0; F1(D) = v1;
}
OP(ps_res) { UNUSED; PAIR(ppc_fres(F0(B)), ppc_fres(F1(B))); }
OP(ps_mul) { UNUSED; PAIR(F0(A) * round25(F0(CC)), F1(A) * round25(F1(CC))); }
OP(ps_rsqrte) { UNUSED; PAIR(ppc_frsqrte(F0(B)), ppc_frsqrte(F1(B))); }
OP(ps_msub) { UNUSED; PAIR(F0(A) * round25(F0(CC)) - F0(B), F1(A) * round25(F1(CC)) - F1(B)); }
OP(ps_madd) { UNUSED; PAIR(F0(A) * round25(F0(CC)) + F0(B), F1(A) * round25(F1(CC)) + F1(B)); }
OP(ps_nmsub) { UNUSED; PAIR(-(F0(A) * round25(F0(CC)) - F0(B)), -(F1(A) * round25(F1(CC)) - F1(B))); }
OP(ps_nmadd) { UNUSED; PAIR(-(F0(A) * round25(F0(CC)) + F0(B)), -(F1(A) * round25(F1(CC)) + F1(B))); }
#define PSQX_EA(upd) (((upd) ? c->r[A] : RA0) + c->r[B])
OP(psq_lx) { UNUSED; psq_load(c, D, PSQX_EA(0), (w >> 10) & 1, (w >> 7) & 7); }
OP(psq_lux) { UNUSED; uint32_t ea = PSQX_EA(1); psq_load(c, D, ea, (w >> 10) & 1, (w >> 7) & 7); c->r[A] = ea; }
OP(psq_stx) { UNUSED; psq_store(c, D, PSQX_EA(0), (w >> 10) & 1, (w >> 7) & 7); }
OP(psq_stux) { UNUSED; uint32_t ea = PSQX_EA(1); psq_store(c, D, ea, (w >> 10) & 1, (w >> 7) & 7); c->r[A] = ea; }
OP(ps_cmp0) { UNUSED; cr_set_f(c, D >> 2, F0(A), F0(B)); }
OP(ps_cmp1) { UNUSED; cr_set_f(c, D >> 2, F1(A), F1(B)); }
#define UNARY(e0, e1) do { double v0 = (e0), v1 = (e1); F0(D) = v0; F1(D) = v1; fprc(c, w); } while (0)
OP(ps_neg) { UNUSED; UNARY(-F0(B), -F1(B)); }
OP(ps_mr) { UNUSED; UNARY(F0(B), F1(B)); }
OP(ps_nabs) { UNUSED; UNARY(-fabs(F0(B)), -fabs(F1(B))); }
OP(ps_abs) { UNUSED; UNARY(fabs(F0(B)), fabs(F1(B))); }
OP(ps_merge00) { UNUSED; UNARY(F0(A), F0(B)); }
OP(ps_merge01) { UNUSED; UNARY(F0(A), F1(B)); }
OP(ps_merge10) { UNUSED; UNARY(F1(A), F0(B)); }
OP(ps_merge11) { UNUSED; UNARY(F1(A), F1(B)); }
OP(dcbz_l) { UNUSED; ppc_dcbz(EAX); }
