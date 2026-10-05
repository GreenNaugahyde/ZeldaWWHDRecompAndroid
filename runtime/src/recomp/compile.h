// Recompiling the game executable to AArch64 object files with LLVM: the device-side
// replacement for tools/recomp/recomp.py + the C compiler. Used by the app (first start) and by
// runtime/tools/recomp_llvm.cpp on a PC.
#pragma once
#include <atomic>
#include <cstddef>
#include <string>

namespace recomp {

// -O3: the game thread spends most of its time in this code (part of the code cache key, so a change
// recompiles once)
constexpr int kDefaultOptLevel = 3;

struct CompileOptions {
    std::string rpxPath;      // game/code/cking.rpx
    std::string opsBitcode;   // ops.c as LLVM bitcode
    std::string hooksText;    // the hooks*.txt files, concatenated
    std::string outDir;       // code_NNN.o are written here
    std::string cpu;          // target CPU ("" = the host's, e.g. on the device; "generic" for a PC build)
    size_t perModule = 1500;  // guest functions per module (and object file)
    int optLevel = kDefaultOptLevel;  // LLVM -O2 or -O3
    unsigned jobs = 0;        // parallel modules (0: hardware threads)
    bool keepExisting = false;  // resume: modules whose object file exists aren't compiled again
    std::atomic<size_t>* modulesDone = nullptr;   // progress (optional)
    std::atomic<size_t>* modulesTotal = nullptr;  // set once the executable is analyzed
    std::atomic<bool>* cancel = nullptr;
};

struct CompileResult {
    size_t functions = 0, modules = 0, instructions = 0, unhandled = 0;
    double seconds = 0;
};

// true and `res` when all object files were written; else false and `err`
bool compile_game(const CompileOptions& opt, CompileResult& res, std::string& err);

// how many modules compile_game will produce (for progress)
size_t module_count(size_t functions, size_t perModule);

// the target features compile_game uses on this device (part of the code cache's key)
std::string host_features();

}  // namespace recomp
