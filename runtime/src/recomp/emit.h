// LLVM IR for recompiled guest functions (the device-side counterpart of tools/recomp/recomp.py's
// C output). Every guest function becomes `void f_XXXXXXXX(Cpu* noalias)`; instructions call the
// ops.c functions (linked in from bitcode) with constant instruction words.
#pragma once
#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "program.h"

namespace llvm {
class LLVMContext;
class Module;
}  // namespace llvm

namespace recomp {

struct Hooks {
    std::set<uint32_t> functions;  // routed through hook_X; the game's code becomes f_X_orig
    std::set<uint32_t> sites;      // site_X(c) runs before the instruction at X
    // hooks.txt syntax: one hex address per line ("@ADDR": instruction site), "#" comments
    void parse(const std::string& text);
};

// symbol of an imported function: imp_<lib without .rpl>_<name> (as tools/recomp/recomp.py)
std::string import_symbol(const Import& imp);

struct EmitStats {
    size_t functions = 0, instructions = 0, unhandled = 0;
};

// One module with the given guest functions (entries of `prog`), plus the ops.c bitcode linked in.
// `opsBitcode`: ops.c compiled with clang -emit-llvm for the target.
std::unique_ptr<llvm::Module> emit_module(llvm::LLVMContext& ctx, const Program& prog, const Hooks& hooks,
                                          const std::vector<uint32_t>& functions, const std::string& opsBitcode,
                                          const std::string& name, EmitStats& stats, std::string& err);

}  // namespace recomp
