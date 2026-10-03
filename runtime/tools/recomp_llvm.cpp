// Host driver for the LLVM recompiler (runtime/src/recomp): cking.rpx -> AArch64 object files.
// Used to validate the device-side recompiler on a PC: the objects replace the clang-compiled
// build/gen/code_*.c in the Android build (CMake -DGEN_OBJECTS=...).
//
// usage: recomp_llvm RPX OPS_BC HOOKS_DIR OUTDIR [functions-per-module] [jobs]
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#include "recomp/compile.h"

static std::string slurp(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main(int argc, char** argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s RPX OPS_BC HOOKS_DIR OUTDIR [functions-per-module] [jobs]\n", argv[0]);
        return 2;
    }
    recomp::CompileOptions opt;
    opt.rpxPath = argv[1];
    opt.opsBitcode = slurp(argv[2]);
    for (const char* f : {"hooks.txt", "hooks_climb.txt", "hooks_mods.txt"}) opt.hooksText += slurp(std::string(argv[3]) + "/" + f) + "\n";
    opt.outDir = argv[4];
    opt.cpu = "generic";
    if (argc > 5) opt.perModule = strtoul(argv[5], nullptr, 10);
    if (argc > 6) opt.jobs = (unsigned)strtoul(argv[6], nullptr, 10);
    recomp::CompileResult res;
    std::string err;
    bool ok = recomp::compile_game(opt, res, err);
    printf("%zu functions, %zu modules, %zu instructions (%zu unhandled) in %.1f s%s%s\n", res.functions, res.modules, res.instructions,
           res.unhandled, res.seconds, ok ? "" : ": ", ok ? "" : err.c_str());
    return ok ? 0 : 1;
}
