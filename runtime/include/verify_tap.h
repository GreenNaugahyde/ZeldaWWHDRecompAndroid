/* Recording taps for the verification harness (tools/verify).
 *
 * A tapped function F runs as an instrumented copy (build/gen/code_tap.c, made by
 * tools/verify/mktap.py): every guest load and store of F's own body and every call it makes
 * are logged. Callees run normally and are not logged. The harness rebuilds F's input
 * (registers + the memory it read) and the effects of each callee as seen by F from the log.
 *
 * File format (little endian, one file per recorded call):
 *   TapHeader, then TapEvent records until type == TAP_END.
 */
#pragma once
#include <stdint.h>

#include "ppc.h"

#define TAP_MAGIC 0x31504154u /* "TAP1" */

typedef struct TapRegs {
    uint32_t r[32];
    uint32_t lr, ctr, xer, fpscr;
    uint8_t cr[32];
    double ps0[32], ps1[32];
    uint32_t gqr[8];
} TapRegs;

typedef struct TapHeader {
    uint32_t magic;
    uint32_t func;
    uint32_t seq;      /* n-th recorded call of this function */
    uint32_t frame;    /* g_Counter.mCounter0 at entry (game step) */
    TapRegs entry;
} TapHeader;

enum {
    TAP_READ = 1,  /* F loaded `size` bytes at ea, value (big-endian value as a number) */
    TAP_WRITE = 2, /* F stored */
    TAP_CALL = 3,  /* F calls target (regs = state at the call) */
    TAP_RET = 4,   /* the call returned (regs = state after it) */
    TAP_END = 5,   /* F returned (regs = state at exit) */
};

typedef struct TapEvent {
    uint8_t type, size, kind, pad;
    uint32_t ea;    /* READ/WRITE: address; CALL: target */
    uint64_t value; /* READ/WRITE */
} TapEvent;
/* TAP_CALL, TAP_RET and TAP_END are followed by a TapRegs */

static inline void tap_regs_from(TapRegs* t, const Cpu* c) {
    for (int i = 0; i < 32; i++) {
        t->r[i] = c->r[i];
        t->cr[i] = c->cr[i];
        t->ps0[i] = c->f[i].ps0;
        t->ps1[i] = c->f[i].ps1;
    }
    t->lr = c->lr; t->ctr = c->ctr; t->fpscr = c->fpscr;
    t->xer = ((uint32_t)c->xer_so << 31) | ((uint32_t)c->xer_ov << 30) | ((uint32_t)c->xer_ca << 29) | c->xer_bc;
    for (int i = 0; i < 8; i++) t->gqr[i] = c->gqr[i];
}
static inline void tap_regs_to(Cpu* c, const TapRegs* t) {
    for (int i = 0; i < 32; i++) {
        c->r[i] = t->r[i];
        c->cr[i] = t->cr[i];
        c->f[i].ps0 = t->ps0[i];
        c->f[i].ps1 = t->ps1[i];
    }
    c->lr = t->lr; c->ctr = t->ctr; c->fpscr = t->fpscr;
    c->xer_so = (t->xer >> 31) & 1; c->xer_ov = (t->xer >> 30) & 1; c->xer_ca = (t->xer >> 29) & 1; c->xer_bc = t->xer & 0x7F;
    for (int i = 0; i < 8; i++) c->gqr[i] = t->gqr[i];
}
