// Guest registers in locals (an idea from the original project's PR #83, "guest registers in C
// locals", there for its C recompiler): see locals.cpp.
#pragma once
#include <cstddef>
#include <string>

namespace llvm {
class Module;
}

namespace recomp {

struct LocalsOptions {
    bool on = true;       // WWHD_RECOMP_LOCALS=0: every register access goes to the Cpu struct (as before)
    bool poison = false;  // WWHD_RECOMP_POISON=1: a checking build (scratch registers get garbage at returns)
};
// from the environment (part of the code cache key: code_switches)
const LocalsOptions& locals_options();
std::string code_switches();

struct LocalsStats {
    size_t functions = 0, fallback = 0;
};

// name of the per-function register copy the emitter allocates (emit.cpp)
constexpr const char* kLocalCpuName = "cpu.local";

// After the ops are inlined: turns each emitted function's register copy into locals with write-backs
// at calls and exits, or back into the Cpu struct where that isn't possible.
void localize_registers(llvm::Module& m, const LocalsOptions& opt, LocalsStats& st);

bool hot_call_order();  // hot functions next to their callers (compile.cpp); in code_switches
}  // namespace recomp
