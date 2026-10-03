// Loading the game code recompiled on the device (compile.h) from the code cache: ORC/JITLink
// links the object files into executable memory (Android doesn't let an app dlopen files it
// wrote), then the function and import tables of recomp_table.h are filled.
#pragma once
#include <cstdint>
#include <string>

namespace recomp {

constexpr uint32_t kSupportedEntryPoint = 0x028EA120;  // cking.rpx of the USA release

// What the cached code was compiled from: the recompiler's version (a hash of its sources, the
// instruction semantics and LLVM; recomp_version.h), the hook lists, the executable's SHA-1 and
// the CPU features. "" if unreadable. Runtime-only changes (renderer, HLE) keep the cache: the
// code links against the runtime's symbols by name at every start.
std::string code_cache_key(const std::string& rpxPath);
// true if `dir` holds a complete cache for `key`
bool code_cache_ready(const std::string& dir, const std::string& key);
// before compiling into `dir`: keeps the modules of an interrupted compile for the same key
// (returns true: resume), else empties `dir`
bool begin_code_cache(const std::string& dir, const std::string& key);
// after compile_game succeeded: marks `dir` complete for `key`
bool finish_code_cache(const std::string& dir, const std::string& key, std::string& err);

// links `dir`/code_*.o and fills g_recomp_funcs / g_recomp_imports; false and `err` on failure
bool load_game_code(const std::string& rpxPath, const std::string& dir, std::string& err);

}  // namespace recomp
