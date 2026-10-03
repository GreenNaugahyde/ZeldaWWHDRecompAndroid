// Instruction classification for the on-device recompiler: which ops.c function implements an
// instruction word, or what kind of control flow it is (handled by the recompiler itself).
#pragma once
#include <cstdint>

namespace recomp {

enum class Kind : uint8_t {
    Op,           // ppcop_<name>(c, w, addr)
    Branch,       // b / ba / bl / bla (op 18)
    BranchCond,   // bc (op 16)
    BranchLr,     // bclr (op 19 xo 16)
    BranchCtr,    // bcctr (op 19 xo 528)
    Unhandled,    // tools/recomp/ppc2c.py raises Unhandled: ppcop_unimplemented
};

struct Decoded {
    Kind kind;
    const char* op;  // ops.c function name without the "ppcop_" prefix (Kind::Op)
};

Decoded decode(uint32_t w);

}  // namespace recomp
