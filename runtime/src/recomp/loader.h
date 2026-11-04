// Loading the game code recompiled on the device (compile.h) from the code cache: ORC/JITLink
// links the object files into executable memory (Android doesn't let an app dlopen files it
// wrote), then the function and import tables of recomp_table.h are filled.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace recomp {

constexpr uint32_t kSupportedEntryPoint = 0x028EA120;  // cking.rpx of the USA release (others: release.h)

// What the cached code was compiled from: the recompiler's version (a hash of its sources, the
// instruction semantics and LLVM; recomp_version.h), the hook lists, the executable's SHA-1 and
// the CPU features. "" if unreadable. Runtime-only changes (renderer, HLE) keep the cache: the
// code links against the runtime's symbols by name at every start.
// code_base_key: all of that; code_cache_key adds the mod packs' code patch (packs.h).
// `features`: another device's CPU features (code compiled here for it, PadActivity); "" = this device's
std::string code_base_key(const std::string& rpxPath, const std::string& features = "");
std::string code_cache_key(const std::string& rpxPath, const std::string& features = "");
// The cache holds object files named by a hash of their content (compile.h) and one manifest per
// code_cache_key (set_<sha1 of the key>: the key and its object files), so switching mod packs back
// and forth needs no compile, and a pack change compiles only the modules it touches.
// true if `dir` holds all code for `key`
bool code_cache_ready(const std::string& dir, const std::string& key);
// before compiling into `dir`: removes the files of the old cache layout (code_NNN.o)
void begin_code_cache(const std::string& dir);
// after compile_game succeeded: the manifest for `key`; then removes object files no longer used by
// the newest few manifests
// the game's code without the randomizer stays in the cache next to the newest sets (switching back needs no
// compile; about one set's room); off: only the newest sets stay
void set_keep_plain_code(bool on);
bool finish_code_cache(const std::string& dir, const std::string& key, const std::vector<std::string>& objects, std::string& err);

// links the objects of the current code_cache_key in `dir` and fills g_recomp_funcs / g_recomp_imports;
// false and `err` on failure
bool load_game_code(const std::string& rpxPath, const std::string& dir, std::string& err);

}  // namespace recomp
