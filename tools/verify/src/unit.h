/* Tables emitted by mkunit.py for one verification unit (unit_<name>.c). */
#pragma once
#include <stdint.h>

#include "ppc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CalleeInfo {
    uint32_t addr;
    const char* name;
    uint32_t imask;   /* argument GPRs compared at the call (bit n = rn) */
    uint32_t fmask;   /* argument FPRs compared (bit n = fn) */
    uint32_t defr;    /* volatile GPRs the callee may write (bit n = rn); ~0 unknown */
    uint32_t deff;    /* volatile FPRs the callee may write */
    uint8_t ptrsz[11]; /* per GPR: pointee size of a pointer argument (0 unknown, 255 constructor storage: not compared) */
    uint8_t outp[11];  /* per GPR: 1 if a non-const pointer (the callee may write through it) */
    void (*real)(Cpu*); /* linked real code (unit option `real`), else NULL */
    uint8_t declared;   /* compare the argument registers the candidate declares (imports) */
    uint8_t retkind;    /* shape of the r3 result: 0 any, 1 zero-extended, 2 sign-extended, 3 constant */
    uint8_t retbits;
    uint32_t retconst;
    uint8_t nstack;     /* argument words passed on the stack (sp+8...) beyond r3-r10 */
} CalleeInfo;

typedef struct OrigFunc {
    uint32_t addr;
    const char* name;      /* WWHD name (names.tsv) */
    const char* gcsym;     /* GameCube mangled symbol, "" if none */
    void (*fn)(Cpu*);
    uint32_t nblocks;      /* coverage points */
    char argtype[11];      /* per GPR r3..r10 from the signature: p ptr, i int, h s16, H u16, c s8, b u8, x unknown */
    uint8_t nflt;          /* float arguments */
    uint32_t livein_r, livein_f; /* argument registers the code reads (dataflow) */
} OrigFunc;

extern const CalleeInfo unit_callees[];
extern const unsigned unit_ncallees;
extern const OrigFunc unit_funcs[];
extern const unsigned unit_nfuncs;
extern const char unit_name[];

#ifdef __cplusplus
}
#endif
